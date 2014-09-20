#include <ngx_http_push_module.h>

#include "store.h"
#include <store/rbtree_util.h>
#include <store/ngx_rwlock.h>
#include <store/ngx_http_push_module_ipc.h>
#include "redis_nginx_adapter.h"
#include "redis_lua_commands.h"

#include <msgpack.h>

#define STR(buf) (buf)->data, (buf)->len
#define BUF(buf) (buf)->start, ((buf)->end - (buf)->start)
   
#define ENQUEUED_DBG "msg %p enqueued.  ref:%i, p:%p n:%p"
#define CREATED_DBG  "msg %p created    ref:%i, p:%p n:%p"
#define FREED_DBG    "msg %p freed.     ref:%i, p:%p n:%p"
#define RESERVED_DBG "msg %p reserved.  ref:%i, p:%p n:%p"
#define RELEASED_DBG "msg %p released.  ref:%i, p:%p n:%p"

#define NGX_HTTP_PUSH_DEFAULT_SUBSCRIBER_POOL_SIZE (5 * 1024)
#define NGX_HTTP_PUSH_DEFAULT_CHANHEAD_CLEANUP_INTERVAL 1000
#define NGX_HTTP_PUSH_CHANHEAD_EXPIRE_SEC 1

//#define DEBUG_SHM_ALLOC 1

static nhpm_channel_head_t *subhash = NULL;

static ngx_event_t         chanhead_cleanup_timer = {0};
static nhpm_llist_timed_t *chanhead_cleanup_head = NULL;
static nhpm_llist_timed_t *chanhead_cleanup_tail = NULL;

static ngx_http_push_channel_queue_t channel_gc_sentinel;
static ngx_slab_pool_t    *ngx_http_push_shpool = NULL;
static ngx_shm_zone_t     *ngx_http_push_shm_zone = NULL;


static void * ngx_http_push_slab_alloc_locked(size_t size, char *label);
static void * ngx_http_push_slab_alloc(size_t size, char *label);
static void ngx_http_push_slab_free_locked(void *ptr);

static ngx_int_t ngx_http_push_store_init_ipc_shm(ngx_int_t workers);

static ngx_int_t ngx_http_push_store_send_worker_message(ngx_http_push_channel_t *channel, ngx_http_push_subscriber_t *subscriber_sentinel, ngx_pid_t pid, ngx_int_t worker_slot, ngx_http_push_msg_t *msg, ngx_int_t status_code);
static void ngx_http_push_store_chanhead_cleanup_timer_handler(ngx_event_t *ev);

static ngx_str_t * ngx_http_push_store_content_type_from_message(ngx_http_push_msg_t *msg, ngx_pool_t *pool);
static ngx_str_t * ngx_http_push_store_etag_from_message(ngx_http_push_msg_t *msg, ngx_pool_t *pool);



static ngx_int_t ngx_http_push_store_init_worker(ngx_cycle_t *cycle) {
  ngx_core_conf_t                *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  
  redis_nginx_init();
  
  chanhead_cleanup_timer.data=NULL;
  chanhead_cleanup_timer.handler=&ngx_http_push_store_chanhead_cleanup_timer_handler;
  chanhead_cleanup_timer.log=ngx_cycle->log;
  
  if(ngx_http_push_store_init_ipc_shm(ccf->worker_processes) == NGX_OK) {
    return ngx_http_push_ipc_init_worker(cycle);
  }
  else {
    return NGX_ERROR;
  }
}

static void redisEchoCallback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply *reply = r;
  ngx_int_t   i;
  //ngx_http_push_channel_t * channel = (ngx_http_push_channel_t *)privdata;
  if (reply == NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS REPLY is NULL");
    return;
  }
  switch(reply->type) {
    case REDIS_REPLY_STATUS:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_STATUS  %s", reply->str);
      break;
      
    case REDIS_REPLY_ERROR:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_ERROR: %s", reply->str);
      break;
      
    case REDIS_REPLY_INTEGER:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_INTEGER: %i", reply->integer);
      break;
      
    case REDIS_REPLY_NIL:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_NIL: nil");
      break;
      
    case REDIS_REPLY_STRING:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_STRING: %s", reply->str);
      break;
      
    case REDIS_REPLY_ARRAY:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_ARRAY: %i", reply->elements);
      for(i=0; i< reply->elements; i++) {
        redisEchoCallback(c, reply->element[i], "  ");
      }
      break;
  }
  //redisAsyncCommand(rds_sub_ctx(), NULL, NULL, "UNSUBSCRIBE channel:%b:pubsub", str(&(channel->id)));
}

static void redisCheckErrorCallback(redisAsyncContext *c, void *r, void *privdata) {
  if(r != NULL && ((redisReply *)r)->type == REDIS_REPLY_ERROR) {
    redisEchoCallback(c, r, privdata);
  }
}

static void redis_load_script_callback(redisAsyncContext *c, void *r, void *privdata) {
  char* (*hashes)[]=(char* (*)[])&nhpm_rds_lua_hashes;
  //char* (*scripts)[]=(char* (*)[])&nhpm_rds_lua_scripts;
  char* (*names)[]=(char* (*)[])&nhpm_rds_lua_script_names;
  uintptr_t i=(uintptr_t) privdata;
  char *hash=(*hashes)[i];

  redisReply *reply = r;
  if (reply == NULL) return;
  switch(reply->type) {
    case REDIS_REPLY_ERROR:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: Failed loading redis lua scripts %s :%s", (*names)[i], reply->str);
      break;
    case REDIS_REPLY_STRING:
      if(ngx_strncmp(reply->str, hash, REDIS_LUA_HASH_LENGTH)!=0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module Redis lua script %s has unexpected hash %s (expected %s)", (*names)[i], reply->str, hash);
      }
      break;
  }
}

static void redisInitScripts(redisAsyncContext *c){
  uintptr_t i;
  char* (*scripts)[]=(char* (*)[])&nhpm_rds_lua_scripts;
  for(i=0; i<sizeof(nhpm_rds_lua_scripts)/sizeof(char*); i++) {
    redisAsyncCommand(c, &redis_load_script_callback, (void *)i, "SCRIPT LOAD %s", (*scripts)[i]);
  }
}

static redisAsyncContext * rds_sub_ctx(void);

static redisAsyncContext * rds_ctx(void){
  static redisAsyncContext *c = NULL;
  if(c==NULL) {
    //init redis
    redis_nginx_open_context((const char *)"localhost", 8537, 1, &c);
    redisInitScripts(c);
  }
  rds_sub_ctx();
  return c;
}

static ngx_int_t msgpack_obj_to_int(msgpack_object *o) {
  switch(o->type) {
    case MSGPACK_OBJECT_POSITIVE_INTEGER:
      return (ngx_int_t) o->via.u64;
    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
      return (ngx_int_t) o->via.i64;
    case MSGPACK_OBJECT_RAW:
      return ngx_atoi((u_char *)o->via.raw.ptr, o->via.raw.size);
    default:
      return 0;
  }
}

#define CHECK_REPLY_STR(reply) ((reply)->type == REDIS_REPLY_STRING)
#define CHECK_REPLY_STRVAL(reply, v) ( CHECK_REPLY_STR(reply) && ngx_strcmp((reply)->str, v) == 0 )
#define CHECK_REPLY_INT(reply) ((reply)->type == REDIS_REPLY_INTEGER)
#define CHECK_REPLY_INTVAL(reply, v) ( CHECK_REPLY_INT(reply) && (reply)->integer == v )
#define CHECK_REPLY_ARRAY_MIN_SIZE(reply, size) ( (reply)->type == REDIS_REPLY_ARRAY && (reply)->elements >= size )

static void redisSubscriberCallback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply *reply = r;
  redisReply *el = NULL;
  ngx_http_push_msg_t msg;
  ngx_buf_t           buf= {0};

  ngx_str_t           channel_id;
  msgpack_unpacked    msgunpack;
  msgpack_object     *cur;
  
  if(reply == NULL) return;
  if(CHECK_REPLY_ARRAY_MIN_SIZE(reply, 3)
    && CHECK_REPLY_STRVAL(reply->element[0], "message")
    && CHECK_REPLY_STR(reply->element[1])
    && CHECK_REPLY_STR(reply->element[2])) {

    msg.expires=0;
    msg.refcount=0;
    msg.buf=NULL;
    //reply->element[1] is channel name
    el = reply->element[2];
    
    msgpack_unpacked_init(&msgunpack);
    if(msgpack_unpack_next(&msgunpack, (char *)el->str, el->len, NULL)) {
      msgpack_object o = msgunpack.data;
      if(o.type==MSGPACK_OBJECT_MAP && o.via.map.size != 0) {
        msgpack_object_kv* p;
        msgpack_object_kv* const pend = o.via.map.ptr + o.via.map.size;
        for(p = o.via.map.ptr; p < pend; ++p) {
          cur=&p->val;
          if(ngx_strncmp("time", p->key.via.raw.ptr, p->key.via.raw.size)==0) {
            switch(cur->type) {
              case MSGPACK_OBJECT_POSITIVE_INTEGER:
                msg.message_time=(time_t) cur->via.u64;
                break;
              case MSGPACK_OBJECT_NEGATIVE_INTEGER:
                msg.message_time=(time_t) cur->via.i64;
                break;
              case MSGPACK_OBJECT_RAW:
                msg.message_time=ngx_atotm((u_char *)cur->via.raw.ptr, cur->via.raw.size);
                break;
              default:
                msg.message_time=0;
            }
          }
          else if(ngx_strncmp("tag", p->key.via.raw.ptr, p->key.via.raw.size)==0) {
            switch(cur->type) {
              case MSGPACK_OBJECT_POSITIVE_INTEGER:
                msg.message_tag=(ngx_int_t) cur->via.u64;
                break;
              case MSGPACK_OBJECT_NEGATIVE_INTEGER:
                msg.message_tag=(ngx_int_t) cur->via.i64;
                break;
              case MSGPACK_OBJECT_RAW:
                msg.message_tag=ngx_atoi((u_char *)cur->via.raw.ptr, cur->via.raw.size);
                break;
              default:
                msg.message_tag=0;
            }
          }
          else if(ngx_strncmp("content_type", p->key.via.raw.ptr, p->key.via.raw.size)==0) {
            if(cur->type == MSGPACK_OBJECT_RAW) {
              msg.content_type.len=cur->via.raw.size;
              msg.content_type.data=(u_char *)cur->via.raw.ptr;
            }
          }
          else if(ngx_strncmp("data", p->key.via.raw.ptr, p->key.via.raw.size)==0) {
            msg.buf=&buf;
            if(cur->type == MSGPACK_OBJECT_RAW) {
              buf.start = buf.pos = (u_char *)cur->via.raw.ptr;
              buf.end = buf.last = (u_char *)(cur->via.raw.ptr + cur->via.raw.size);
              buf.memory = 1;
              buf.last_buf = 1;
              buf.last_in_chain = 1;
            }
          }
          else if(ngx_strncmp("channel", p->key.via.raw.ptr, p->key.via.raw.size)==0) {
            channel_id.len=cur->via.raw.size;
            channel_id.data=(u_char *)cur->via.raw.ptr;
          }
        }
      }
    }
    else {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: invalid msgpack message from redis");
    }
    msgpack_unpacked_destroy(&msgunpack);
    
    
    /*
    //message stuff
    char      *str=NULL, *a=NULL, *b=NULL;
    ngx_str_t  s;
    str=el->str;
    msg.buf=&buf;
    
    if((a=ngx_strchr(str, ':'))==NULL) {
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "invalid message, first ':' not found");
      return;
    }
    
    if(ngx_strncmp("delete", str, 6)==0) {
      a++;
      s.data=(u_char *)a;
      s.len=el->len - (a-str);
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "got mesage: delete channel %V", &s);
      return;
    }
    
    msg.message_time=ngx_atotm((u_char *)str, (a-str));

    a++;
    if((b=ngx_strchr(a, ':'))==NULL) {
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "invalid message, second ':' not found");
      return;
    }
    msg.message_tag=ngx_atoi((u_char *)a, (b-a));

    a=b+1;
    if((b=ngx_strchr(a, ':'))==NULL) {
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "invalid message, third ':' not found");
      return;
    }
    msg.content_type.data = (u_char *)a;
    msg.content_type.len = (b-a);
    
    a=b+1;
    if(el->len < (a - str)) {
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "...where's the message?");
      return;
    }
    buf.start = buf.pos = (u_char *)a;
    buf.end =buf.last = (u_char *)str + sizeof(u_char)*(el->len);
    buf.memory = 1;
    buf.last_buf = 1;
    buf.last_in_chain = 1;
    */
  }
  else if(CHECK_REPLY_ARRAY_MIN_SIZE(reply, 3)
    && CHECK_REPLY_STRVAL(reply->element[0], "subscribe")
    && CHECK_REPLY_STR(reply->element[1])
    && CHECK_REPLY_INT(reply->element[2])) {

    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "subscribed to %s (%i total)", reply->element[1]->str, reply->element[1]->integer);
  }
  else {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "no message, something else");
    redisEchoCallback(c,r,privdata);
  }
}

static u_char subscriber_id[255] = "";
static u_char subscriber_channel[255] = "";
static redisAsyncContext * rds_sub_ctx(void){
  static redisAsyncContext *c = NULL;
  if(subscriber_id[0] == 0) {
    ngx_snprintf(subscriber_id, 255, "worker:%i:time:%i", ngx_pid, ngx_time());
    ngx_snprintf(subscriber_channel, 255, "nginx_push_module:%s", subscriber_id);
  }
  if(c==NULL) {
    //init redis
    redis_nginx_open_context((const char *)"localhost", 8537, 1, &c);
    redisAsyncCommand(c, redisSubscriberCallback, NULL, "SUBSCRIBE %s", subscriber_channel);
  }
  return c;
}

//will be called once per worker
static ngx_int_t ngx_http_push_store_init_ipc_shm(ngx_int_t workers) {
  ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *) ngx_http_push_shm_zone->shm.addr;
  ngx_http_push_shm_data_t       *d = (ngx_http_push_shm_data_t *) ngx_http_push_shm_zone->data;
  ngx_http_push_worker_msg_sentinel_t     *worker_messages=NULL;
  ngx_shmtx_lock(&shpool->mutex);
  if(d->ipc==NULL) {
    //ipc uninitialized. get it done!
    if((worker_messages = ngx_http_push_slab_alloc_locked(sizeof(*worker_messages)*NGX_MAX_PROCESSES, "IPC worker message sentinel array"))==NULL) {
      ngx_shmtx_unlock(&shpool->mutex);
      return NGX_ERROR;
    }
    d->ipc=worker_messages;
  }
  else {
    worker_messages=d->ipc;
  }
  
  ngx_queue_init(&worker_messages[ngx_process_slot].queue);
  ngx_rwlock_init(&worker_messages[ngx_process_slot].lock);
  
  ngx_shmtx_unlock(&shpool->mutex);
  return NGX_OK;
}


static ngx_int_t ngx_http_push_channel_collector(ngx_http_push_channel_t * channel) {
  if((ngx_http_push_clean_channel_locked(channel))!=NULL) { //we're up for deletion
    ngx_http_push_channel_queue_t *trashy;
    if((trashy = ngx_alloc(sizeof(*trashy), ngx_cycle->log))!=NULL) {
      //yeah, i'm allocating memory during garbage collection. sue me.
      trashy->channel=channel;
      ngx_queue_insert_tail(&channel_gc_sentinel.queue, &trashy->queue);
      return NGX_OK;
    }
    return NGX_ERROR;
  }
  return NGX_OK;
}

static void ngx_http_push_store_lock_shmem(void){
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
}
static void ngx_http_push_store_unlock_shmem(void){
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
}

//garbage-collecting slab allocator
static void * ngx_http_push_slab_alloc_locked(size_t size, char *label) {
  void  *p;
  if((p = ngx_slab_alloc_locked(ngx_http_push_shpool, size))==NULL) {
    ngx_http_push_channel_queue_t *ccur, *cnext;
    ngx_uint_t                  collected = 0;
    //failed. emergency garbage sweep, then.
    
    //collect channels
    ngx_queue_init(&channel_gc_sentinel.queue);
    ngx_http_push_walk_rbtree(ngx_http_push_channel_collector, ngx_http_push_shm_zone);
    for(ccur=(ngx_http_push_channel_queue_t *)ngx_queue_next(&channel_gc_sentinel.queue); ccur != &channel_gc_sentinel; ccur=cnext) {
      cnext = (ngx_http_push_channel_queue_t *)ngx_queue_next(&ccur->queue);
      ngx_http_push_delete_channel_locked(ccur->channel, ngx_http_push_shm_zone);
      ngx_free(ccur);
      collected++;
    }
    
    //todo: collect worker messages maybe
    
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "push module: out of shared memory. emergency garbage collection deleted %ui unused channels.", collected);
    
    p = ngx_slab_alloc_locked(ngx_http_push_shpool, size);
  }
#if (DEBUG_SHM_ALLOC == 1)
  if (p != NULL) {
    if(label==NULL)
      label="none";
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "shpool alloc addr %p size %ui label %s", p, size, label);
  }
#endif
  return p;
}

static void * ngx_http_push_slab_alloc(size_t size, char *label) {
  void * p;
  ngx_http_push_store_lock_shmem();
  p= ngx_http_push_slab_alloc_locked(size, label);
  ngx_http_push_store_unlock_shmem();
  return p;
}

static void ngx_http_push_slab_free_locked(void *ptr) {
  ngx_slab_free_locked(ngx_http_push_shpool, ptr);
  #if (DEBUG_SHM_ALLOC == 1)
  ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "shpool free addr %p", ptr);
  #endif
}
/*
static void ngx_http_push_slab_free(void *ptr) {
  ngx_http_push_store_lock_shmem();
  ngx_http_push_slab_free_locked(ptr);
  ngx_http_push_store_unlock_shmem();
}*/

//shpool is assumed to be locked.
static ngx_http_push_msg_t *ngx_http_push_get_latest_message_locked(ngx_http_push_channel_t * channel) {
  ngx_queue_t                    *sentinel = &channel->message_queue->queue; 
  if(ngx_queue_empty(sentinel)) {
    return NULL;
  }
  ngx_queue_t                    *qmsg = ngx_queue_last(sentinel);
  return ngx_queue_data(qmsg, ngx_http_push_msg_t, queue);
}

//shpool must be locked. No memory is freed. O(1)
static ngx_http_push_msg_t *ngx_http_push_get_oldest_message_locked(ngx_http_push_channel_t * channel) {
  ngx_queue_t                    *sentinel = &channel->message_queue->queue; 
  if(ngx_queue_empty(sentinel)) {
    return NULL;
  }
  ngx_queue_t                    *qmsg = ngx_queue_head(sentinel);
  return ngx_queue_data(qmsg, ngx_http_push_msg_t, queue);
}

static void ngx_http_push_store_reserve_message_locked(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg) {
  if(msg == NULL) {
    return;
  }
  msg->refcount++;
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, RESERVED_DBG, msg, msg->refcount, msg->queue.prev, msg->queue.next);
  //we need a refcount because channel messages MAY be dequed before they are used up. It thus falls on the IPC stuff to free it.
}

/*
static void ngx_http_push_store_reserve_message_num_locked(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg, ngx_int_t reservations) {
  if(msg == NULL) {
    return;
  }
  msg->refcount+=reservations;
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, RESERVED_DBG, msg, msg->refcount, msg->queue.prev, msg->queue.next);
  //we need a refcount because channel messages MAY be dequed before they are used up. It thus falls on the IPC stuff to free it.
}
*/

static void ngx_http_push_store_reserve_message(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg) {
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  ngx_http_push_store_reserve_message_locked(channel, msg);
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  //we need a refcount because channel messages MAY be dequed before they are used up. It thus falls on the IPC stuff to free it.
}



//free memory for a message. 
static ngx_inline void ngx_http_push_free_message_locked(ngx_http_push_msg_t *msg, ngx_slab_pool_t *shpool) {
  if(msg->buf->file!=NULL) {
    // i'd like to release the shpool lock here while i do stuff to this file, but that 
    // might unlock during channel rbtree traversal, which is Bad News.
    if(msg->buf->file->fd!=NGX_INVALID_FILE) {
      ngx_close_file(msg->buf->file->fd);
    }
    ngx_delete_file(msg->buf->file->name.data); //should I care about deletion errors? doubt it.
  }
  ngx_http_push_slab_free_locked(msg->buf); //separate block, remember?
  ngx_http_push_slab_free_locked(msg);
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, FREED_DBG, msg, msg->refcount, msg->queue.prev, msg->queue.next);
  if(msg->refcount < 0) { //something worth exploring went wrong
    raise(SIGSEGV);
  }
  ((ngx_http_push_shm_data_t *) ngx_http_push_shm_zone->data)->messages--;
}

// remove a message from queue and free all associated memory. assumes shpool is already locked.
static ngx_int_t ngx_http_push_delete_message_locked(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg, ngx_int_t force) {
  if (msg==NULL) {
    return NGX_OK;
  }
  if(channel!=NULL) {
    ngx_queue_remove(&msg->queue);
    channel->messages--;
  }
  if(msg->refcount<=0 || force) {
    //nobody needs this message, or we were forced at integer-point to delete
    ngx_http_push_free_message_locked(msg, ngx_http_push_shpool);
  }
  return NGX_OK;
}

static void ngx_http_push_store_release_message_locked(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg) {
  if(msg == NULL) {
    return;
  }
  msg->refcount--;
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, RELEASED_DBG, msg, msg->refcount, msg->queue.prev, msg->queue.next);
  if(msg->queue.next==NULL && msg->refcount<=0) { 
    //message had been dequeued and nobody needs it anymore
    ngx_http_push_free_message_locked(msg, ngx_http_push_shpool);
  }
  if(channel != NULL && channel->messages > msg->delete_oldest_received_min_messages && ngx_http_push_get_oldest_message_locked(channel) == msg) {
    ngx_http_push_delete_message_locked(channel, msg, 0);
  }
}

static void ngx_http_push_store_release_message(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg) {
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  ngx_http_push_store_release_message_locked(channel, msg);
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
}

static ngx_int_t ngx_http_push_delete_message(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg, ngx_int_t force) {
  ngx_int_t ret;
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  ret = ngx_http_push_delete_message_locked(channel, msg, force);
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  return ret;
}


/** find message with entity tags matching those of the request r.
  * @param r subscriber request
  */
static ngx_http_push_msg_t * ngx_http_push_find_message_locked(ngx_http_push_channel_t *channel, ngx_http_push_msg_id_t *msgid, ngx_int_t *status) {
  //TODO: consider using an RBTree for message storage.
  ngx_queue_t                    *sentinel = &channel->message_queue->queue;
  ngx_queue_t                    *cur = ngx_queue_head(sentinel);
  ngx_http_push_msg_t            *msg;
  
  time_t                          time = msgid->time;
  ngx_int_t                       tag = msgid->tag;
  
  //channel's message buffer empty?
  if(channel->messages==0) {
    *status=NGX_HTTP_PUSH_MESSAGE_EXPECTED; //wait.
    return NULL;
  }
  
  // do we want a future message?
  msg = ngx_queue_data(sentinel->prev, ngx_http_push_msg_t, queue); 
  if(time <= msg->message_time) { //that's an empty check (Sentinel's values are zero)
    if(time == msg->message_time) {
      if(tag >= msg->message_tag) {
        *status=NGX_HTTP_PUSH_MESSAGE_EXPECTED;
        return NULL;
      }
    }
  }
  else {
    *status=NGX_HTTP_PUSH_MESSAGE_EXPECTED;
    return NULL;
  }
  
  while(cur!=sentinel) {
    msg = ngx_queue_data(cur, ngx_http_push_msg_t, queue);
    if (time < msg->message_time) {
      *status = NGX_HTTP_PUSH_MESSAGE_FOUND;
      return msg;
    }
    else if(time == msg->message_time) {
      while (tag >= msg->message_tag  && time == msg->message_time && ngx_queue_next(cur)!=sentinel) {
        cur=ngx_queue_next(cur);
        msg = ngx_queue_data(cur, ngx_http_push_msg_t, queue);
      }
      if(time == msg->message_time && tag < msg->message_tag) {
        *status = NGX_HTTP_PUSH_MESSAGE_FOUND;
        return msg;
      }
      continue;
    }
    cur=ngx_queue_next(cur);
  }
  *status = NGX_HTTP_PUSH_MESSAGE_EXPIRED; //message too old and was not found.
  return NULL;
}

static ngx_http_push_channel_t * ngx_http_push_store_find_channel(ngx_str_t *id, time_t channel_timeout, ngx_int_t (*callback)(ngx_http_push_channel_t *channel)) {
  //get the channel and check channel authorization while we're at it.
  ngx_http_push_channel_t        *channel;
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  channel = ngx_http_push_find_channel(id, channel_timeout, ngx_http_push_shm_zone);
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  if(callback!=NULL) {
    callback(channel);
  }
  return channel;
}


static void ngx_http_push_store_respond_subs_cleanup(void *data) {
  nhpm_subscriber_cleanup_t *cln=(nhpm_subscriber_cleanup_t *)data;
  //nhpm_channel_head_t       *head=cln->head;
  cln->count--;
  if(cln->count <= 0) {
    //we're done here
    ngx_destroy_pool(cln->pool);
  }
}

static ngx_pool_t *chanhead_ensure_pool_exists(nhpm_channel_head_t *chanhead) {
  if(chanhead->pool==NULL) {
    if((chanhead->pool=ngx_create_pool(NGX_HTTP_PUSH_DEFAULT_SUBSCRIBER_POOL_SIZE, ngx_cycle->log))==NULL) {
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "can't allocate memory for channel subscriber pool");
    }
  }
  return chanhead->pool;
}

static ngx_int_t ngx_http_push_store_publish_raw(ngx_str_t *channel_id, ngx_http_push_msg_t *msg, ngx_int_t status_code, const ngx_str_t *status_line){
  nhpm_channel_head_t        *head;
  nhpm_subscriber_t          *sub;
  nhpm_subscriber_cleanup_t  *clndata;
  
  nhpm_llist_timed_t         *chanhead_cleanlink;
  
  ngx_buf_t                  *buffer=NULL;
  ngx_str_t                  *etag, *content_type;
  ngx_chain_t                *chain;
  
  CHANNEL_HASH_FIND(channel_id, head);
  if(head==NULL) {
    return NGX_HTTP_PUSH_MESSAGE_QUEUED;
  }
  chanhead_ensure_pool_exists(head);
  
  if ((clndata=ngx_palloc(head->pool, sizeof(*clndata))) != NULL) {
    clndata->head=head;
    clndata->count=head->sub_count;
    clndata->pool =head->pool;
  }
  else {
    return NGX_ERROR;
  }
  if(msg!=NULL) {
    etag = ngx_http_push_store_etag_from_message(msg, head->pool);
    content_type = ngx_http_push_store_content_type_from_message(msg, head->pool);
    chain = ngx_http_push_create_output_chain(msg->buf, head->pool, ngx_cycle->log);
    if(chain==NULL) {
      return NGX_ERROR;
    }
    buffer = chain->buf;
    buffer->recycled = 1;
  }
  
  for(sub=head->sub; sub!=NULL; sub=sub->next) {
    ngx_chain_t               *rchain;
    ngx_buf_t                 *rbuffer;
    ngx_http_request_t        *r;
    ngx_http_cleanup_t        *cln;


    r=(ngx_http_request_t *)sub->subscriber;
    if ((cln=ngx_http_cleanup_add(r, 0)) == NULL) {
      return NGX_ERROR;
    }
    cln->handler = &ngx_http_push_store_respond_subs_cleanup;
    cln->data = clndata;
    
    if(msg!=NULL) {
      //each response needs its own chain and buffer, though the buffer contents can be shared
      rchain = ngx_pcalloc(sub->pool, sizeof(*rchain));
      rbuffer = ngx_pcalloc(sub->pool, sizeof(*rbuffer));
      rchain->next = NULL;
      rchain->buf = rbuffer;
      ngx_memcpy(rbuffer, buffer, sizeof(*buffer));
      
      ngx_http_finalize_request(r, ngx_http_push_prepare_response_to_subscriber_request(r, rchain, content_type, etag, msg->message_time));
    }
    else {
      ngx_http_finalize_request(r, ngx_http_push_respond_status_only(r, status_code, status_line));
    }
  }
  
  redisAsyncCommand(rds_ctx(), &redisCheckErrorCallback, NULL, "EVALSHA %s 0 %b %i", nhpm_rds_lua_hashes.subscriber_count, STR(channel_id), -(head->sub_count));
  
  head->pool=NULL;
  head->sub=NULL;
  head->sub_count=0;
  
  if(head->cleanlink==NULL) {
    //add channel head to cleanup list
    if((chanhead_cleanlink=ngx_alloc(sizeof(*chanhead_cleanlink), ngx_cycle->log))==NULL) {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "can't allocate memory for channel subscriber head cleanup list element");
      return NGX_ERROR;
    }
    chanhead_cleanlink->data=(void *)head;
    chanhead_cleanlink->time=ngx_time();
    chanhead_cleanlink->prev=chanhead_cleanup_tail;
    if(chanhead_cleanup_tail != NULL) {
      chanhead_cleanup_tail->next=chanhead_cleanlink;
    }
    chanhead_cleanlink->next=NULL;
    chanhead_cleanup_tail=chanhead_cleanlink;
    if(chanhead_cleanup_head==NULL) {
      chanhead_cleanup_head = chanhead_cleanlink;
    }
    head->cleanlink=chanhead_cleanlink;
  }
  
  //initialize cleanup timer
  if(!chanhead_cleanup_timer.timer_set) {
    ngx_add_timer(&chanhead_cleanup_timer, NGX_HTTP_PUSH_DEFAULT_CHANHEAD_CLEANUP_INTERVAL);
  }
  
  return NGX_HTTP_PUSH_MESSAGE_RECEIVED;
}

static void ngx_http_push_store_chanhead_cleanup_timer_handler(ngx_event_t *ev) {
  nhpm_llist_timed_t    *cur, *next;
  nhpm_channel_head_t   *ch = NULL;

  ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "ngx_http_push_store_chanhead_cleanup_timer_handler");
  for(cur=chanhead_cleanup_head; cur != NULL; cur=next) {
    next=cur->next;
    if(ngx_time() - cur->time > NGX_HTTP_PUSH_CHANHEAD_EXPIRE_SEC) {
      ch = (nhpm_channel_head_t *)cur->data;
      if (ch->sub==NULL) { //still no subscribers here
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "channel_head is empty and expired. delete %p.", ch);
        CHANNEL_HASH_DEL(ch);
        ngx_free(ch);
        ngx_free(cur);
      }
      else {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "channel_head is still being used.");
        ngx_free(cur);
      }
    }
    else {
      break;
    }
  }
  chanhead_cleanup_head=cur;
  if (cur==NULL) { //we went all the way to the end
    chanhead_cleanup_tail=NULL;
  }
  else {
    cur->prev=NULL;
  }

  if (!(ngx_quit || ngx_terminate || ngx_exiting || chanhead_cleanup_head==NULL)) {
    ngx_add_timer(ev, NGX_HTTP_PUSH_DEFAULT_CHANHEAD_CLEANUP_INTERVAL);
  }
}

static ngx_int_t ngx_http_push_store_delete_channel(ngx_str_t *channel_id) {
  ngx_http_push_channel_t        *channel;
  ngx_http_push_msg_t            *msg, *sentinel;
  
  
  redisAsyncCommand(rds_ctx(), &redisEchoCallback, NULL, "EVALSHA %s 0 %b", nhpm_rds_lua_hashes.delete, STR(channel_id));
  
  ngx_http_push_store_lock_shmem();
  channel = ngx_http_push_find_channel(channel_id, NGX_HTTP_PUSH_DEFAULT_CHANNEL_TIMEOUT, ngx_http_push_shm_zone);
  if (channel == NULL) {
    ngx_http_push_store_unlock_shmem();
    return NGX_OK;
  }
  sentinel = channel->message_queue;
  msg = sentinel;
        
  while((msg=(ngx_http_push_msg_t *)ngx_queue_next(&msg->queue))!=sentinel) {
    //force-delete all the messages
    ngx_http_push_delete_message_locked(NULL, msg, 1);
  }
  channel->messages=0;
  
  //410 gone
  ngx_http_push_store_unlock_shmem();
  
  ngx_http_push_store_publish_raw(channel_id, NULL, NGX_HTTP_GONE, &NGX_HTTP_PUSH_HTTP_STATUS_410);
  
  ngx_http_push_store_lock_shmem();
  ngx_http_push_delete_channel_locked(channel, ngx_http_push_shm_zone);
  ngx_http_push_store_unlock_shmem();
  return NGX_OK;
}

static ngx_http_push_channel_t * ngx_http_push_store_get_channel(ngx_str_t *id, time_t channel_timeout, ngx_int_t (*callback)(ngx_http_push_channel_t *channel)) {
  //get the channel and check channel authorization while we're at it.
  ngx_http_push_channel_t        *channel;
  
  
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  channel = ngx_http_push_get_channel(id, channel_timeout, ngx_http_push_shm_zone);
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  if(channel==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: unable to allocate memory for new channel");
  }
  if(callback!=NULL) {
    callback(channel);
  }
  return channel;
}

static ngx_http_push_msg_t * ngx_http_push_store_get_channel_message(ngx_http_push_channel_t *channel, ngx_http_push_msg_id_t *msgid, ngx_int_t *msg_search_outcome, ngx_http_push_loc_conf_t *cf) {
  ngx_http_push_msg_t *msg;
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  msg = ngx_http_push_find_message_locked(channel, msgid, msg_search_outcome);
  if(*msg_search_outcome == NGX_HTTP_PUSH_MESSAGE_FOUND) {
    ngx_http_push_store_reserve_message_locked(channel, msg);
  }
  channel->last_seen = ngx_time();
  channel->expires = ngx_time() + cf->channel_timeout;
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  return msg;
}

static void * ngx_nhpm_alloc(size_t size) {
  return ngx_alloc(size, ngx_cycle->log);
}

static ngx_http_push_msg_t * msg_from_get_message_reply(redisReply *r, ngx_int_t offset, void *(*allocator)(size_t size)) {
  ngx_http_push_msg_t *msg=NULL;
  ngx_buf_t           *buf=NULL;
  redisReply         **els = r->element;
  size_t len = 0, content_type_len = 0;

  if(CHECK_REPLY_ARRAY_MIN_SIZE(r, offset + 4)
   && CHECK_REPLY_INT(els[offset])     //id - time
   && CHECK_REPLY_INT(els[offset+1])   //id - tag
   && CHECK_REPLY_STR(els[offset+2])   //message
   && CHECK_REPLY_STR(els[offset+3])){ //content-type
    len=els[offset+2]->len;
    content_type_len=els[offset+3]->len;
    if((msg=allocator(sizeof(*msg) + sizeof(ngx_buf_t) + len + content_type_len))==NULL) {
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "push module: can't allocate memory for message from redis reply");
      return NULL;
    }
    ngx_memzero(msg, sizeof(*msg)+sizeof(ngx_buf_t));
    //set up message buffer;
    msg->buf = (void *)(&msg[1]);
    buf = msg->buf;
    buf->start = buf->pos = (void *)(&buf[1]);
    buf->end = buf->last = buf->start + (u_char)len;
    ngx_memcpy(buf->start, els[3]->str, len);
    buf->memory = 1;
    buf->last_buf = 1;
    buf->last_in_chain = 1;
    
    if(content_type_len>0) {
      msg->content_type.len=content_type_len;
      msg->content_type.data=buf->end;
      ngx_memcpy(msg->content_type.data, els[4]->str, content_type_len);
    }
    
    msg->message_time = els[1]->integer;
    msg->message_tag = els[2]->integer;
    return msg;
  }
  else {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "push module: invalid message redis reply");
    return NULL;
  }
}

typedef struct {
  ngx_http_request_t  *r;
  ngx_str_t           *channel_id;
  ngx_http_push_msg_id_t *msg_id;
  ngx_int_t (*callback)(ngx_http_push_msg_t *msg, ngx_int_t msg_search_outcome, ngx_http_request_t *r);
} redis_get_message_data_t;

static void redis_get_message_callback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply                *reply= r;
  redis_get_message_data_t  *d= (redis_get_message_data_t *)privdata;
  ngx_http_push_msg_t       *msg=NULL;
  
  
  //output: result_code, msg_time, msg_tag, message, content_type,  channel_subscriber_count
  // result_code can be: 200 - ok, 403 - channel not found, 404 - not found, 410 - gone, 418 - not yet available
  
  if ( !CHECK_REPLY_ARRAY_MIN_SIZE(reply, 1) || !CHECK_REPLY_INT(reply->element[0]) ) {
    //no good
    ngx_free(d);
    return;
  }
  
  switch(reply->element[0]->integer) {
    case 200: //ok
      if((msg=msg_from_get_message_reply(reply, 1, &ngx_nhpm_alloc))) {
        d->callback(msg, NGX_HTTP_PUSH_MESSAGE_FOUND, d->r);
      }
      break;
    case 403: //channel not found
    case 404: //not found
      d->callback(NULL, NGX_HTTP_PUSH_MESSAGE_NOTFOUND, d->r);
      break;
    case 410: //gone
      d->callback(NULL, NGX_HTTP_PUSH_MESSAGE_EXPIRED, d->r);
      break;
    case 418: //not yet available
      d->callback(NULL, NGX_HTTP_PUSH_MESSAGE_EXPECTED, d->r);
      break;
  }
  
  ngx_free(d);
}

static ngx_int_t ngx_http_push_store_async_get_message(ngx_str_t *channel_id, ngx_http_push_msg_id_t *msg_id, ngx_http_request_t *r, ngx_int_t (*callback)(ngx_http_push_msg_t *msg, ngx_int_t msg_search_outcome, ngx_http_request_t *r)) {
  redis_get_message_data_t           *d=NULL;
  if(callback==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "no callback given for async get_message. someone's using the API wrong!");
    return NGX_ERROR;
  }
  if((d=ngx_alloc(sizeof(*d), ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "Failed to allocate memory for some callback data");
    return NGX_ERROR;
  }
  d->r = r;
  d->channel_id = channel_id;
  d->msg_id = msg_id;
  d->callback = callback;
  
  //input:  keys: [], values: [channel_id, msg_time, msg_tag, no_msgid_order, create_channel_ttl, subscriber_channel]
  //subscriber channel is not given, because we don't care to subscribe
  redisAsyncCommand(rds_ctx(), &redis_get_message_callback, (void *)d, "EVALSHA %s 0 %b %i %i %s", nhpm_rds_lua_hashes.get_message, STR(channel_id), msg_id->time, msg_id->tag, "FILO", 0);
  return NGX_OK; //async only now!
}

// shared memory zone initializer
static ngx_int_t  ngx_http_push_init_shm_zone(ngx_shm_zone_t * shm_zone, void *data) {
  if(data) { /* zone already initialized */
    shm_zone->data = data;
    return NGX_OK;
  }

  ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
  ngx_rbtree_node_t              *sentinel;
  ngx_http_push_shm_data_t       *d;
  
  ngx_http_push_shpool = shpool; //we'll be using this a bit.
  #if (DEBUG_SHM_ALLOC == 1)
  ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "ngx_http_push_shpool start %p size %i", shpool->start, (u_char *)shpool->end - (u_char *)shpool->start);
  #endif
  
  if ((d = (ngx_http_push_shm_data_t *)ngx_http_push_slab_alloc(sizeof(*d), "shm data")) == NULL) { //shm_data
    return NGX_ERROR;
  }
  d->channels=0;
  d->messages=0;
  shm_zone->data = d;
  d->ipc=NULL;
  //initialize rbtree
  if ((sentinel = ngx_http_push_slab_alloc(sizeof(*sentinel), "channel rbtree sentinel"))==NULL) {
    return NGX_ERROR;
  }
  ngx_rbtree_init(&d->tree, sentinel, ngx_http_push_rbtree_insert);
  return NGX_OK;
}

//shared memory
static ngx_str_t  ngx_push_shm_name = ngx_string("push_module"); //shared memory segment name
static ngx_int_t  ngx_http_push_set_up_shm(ngx_conf_t *cf, size_t shm_size) {
  ngx_http_push_shm_zone = ngx_shared_memory_add(cf, &ngx_push_shm_name, shm_size, &ngx_http_push_module);
  if (ngx_http_push_shm_zone == NULL) {
    return NGX_ERROR;
  }
  ngx_http_push_shm_zone->init = ngx_http_push_init_shm_zone;
  ngx_http_push_shm_zone->data = (void *) 1; 
  return NGX_OK;
}

//initialization
static ngx_int_t ngx_http_push_store_init_module(ngx_cycle_t *cycle) {
  ngx_core_conf_t                *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  ngx_http_push_worker_processes = ccf->worker_processes;
  //initialize our little IPC
  return ngx_http_push_init_ipc(cycle, ngx_http_push_worker_processes);
}

static ngx_int_t ngx_http_push_store_init_postconfig(ngx_conf_t *cf) {
  ngx_http_push_main_conf_t *conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_push_module);

  //initialize shared memory
  size_t                       shm_size;
  if(conf->shm_size==NGX_CONF_UNSET_SIZE) {
    conf->shm_size=NGX_HTTP_PUSH_DEFAULT_SHM_SIZE;
  }
  shm_size = ngx_align(conf->shm_size, ngx_pagesize);
  if (shm_size < 8 * ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "The push_max_reserved_memory value must be at least %udKiB", (8 * ngx_pagesize) >> 10);
        shm_size = 8 * ngx_pagesize;
    }
  if(ngx_http_push_shm_zone && ngx_http_push_shm_zone->shm.size != shm_size) {
    ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Cannot change memory area size without restart, ignoring change");
  }
  ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "Using %udKiB of shared memory for push module", shm_size >> 10);
  
  
  //initialize hash for channels
  
  return ngx_http_push_set_up_shm(cf, shm_size);
}

static void ngx_http_push_store_create_main_conf(ngx_conf_t *cf, ngx_http_push_main_conf_t *mcf) {
  mcf->shm_size=NGX_CONF_UNSET_SIZE;
}

//great justice appears to be at hand
static ngx_int_t ngx_http_push_movezig_channel_locked(ngx_http_push_channel_t * channel) {
  ngx_queue_t                 *sentinel = &channel->message_queue->queue;
  ngx_http_push_msg_t         *msg=NULL;
  while(!ngx_queue_empty(sentinel)) {
    msg = ngx_queue_data(ngx_queue_head(sentinel), ngx_http_push_msg_t, queue);
    ngx_http_push_delete_message_locked(channel, msg, 1);
  }
  return NGX_OK;
}
static ngx_int_t ngx_http_push_store_channel_subscribers(ngx_http_push_channel_t * channel) {
  ngx_int_t subs;
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  subs = channel->subscribers;
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  return subs;
}

static ngx_int_t ngx_http_push_store_channel_worker_subscribers(ngx_http_push_subscriber_t * worker_sentinel) {
  ngx_http_push_subscriber_t *cur;
  ngx_int_t count=0;
  cur=(ngx_http_push_subscriber_t *)ngx_queue_head(&worker_sentinel->queue);
  while(cur!=worker_sentinel) {
    count++;
    cur=(ngx_http_push_subscriber_t *)ngx_queue_next(&cur->queue);
  }
  return count;
}

static ngx_http_push_subscriber_t *ngx_http_push_store_channel_next_subscriber(ngx_http_push_channel_t *channel, ngx_http_push_subscriber_t *sentinel, ngx_http_push_subscriber_t *cur, int release_previous) {
  ngx_http_push_subscriber_t *next;
  if(cur==NULL) {
    next=(ngx_http_push_subscriber_t *)ngx_queue_head(&sentinel->queue);
  }
  else{
    next=(ngx_http_push_subscriber_t *)ngx_queue_next(&cur->queue);
    if(release_previous==1 && cur!=sentinel) {
      //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "freeing subscriber cursor at %p.", cur);
      ngx_pfree(ngx_http_push_pool, cur);
    }
  }
  return next!=sentinel ? next : NULL;
}

static ngx_int_t ngx_http_push_store_channel_release_subscriber_sentinel(ngx_http_push_channel_t *channel, ngx_http_push_subscriber_t *sentinel) {
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "freeing subscriber sentinel at %p.", sentinel);
  ngx_pfree(ngx_http_push_pool, sentinel);
  return NGX_OK;
}

static void ngx_http_push_store_exit_worker(ngx_cycle_t *cycle) {
  ngx_http_push_ipc_exit_worker(cycle);
}

static void ngx_http_push_store_exit_master(ngx_cycle_t *cycle) {
  //destroy channel tree in shared memory
  ngx_http_push_walk_rbtree(ngx_http_push_movezig_channel_locked, ngx_http_push_shm_zone);
  //deinitialize IPC
  ngx_http_push_shutdown_ipc(cycle);
}

static ngx_int_t ngx_http_push_store_subscribe_new(ngx_str_t *channel_id, ngx_http_request_t *r) {
  //this is the new shit
  ngx_http_push_loc_conf_t  *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_module);
  nhpm_channel_head_t       *chanhead = NULL;
  ngx_str_t                 *chan_id = NULL;
  nhpm_subscriber_t         *nextsub;
  
  CHANNEL_HASH_FIND(channel_id, chanhead);
  if(chanhead==NULL) {
    chanhead=(nhpm_channel_head_t *)ngx_alloc(sizeof(*chanhead) + sizeof(ngx_str_t) + channel_id->len, ngx_cycle->log);
    if(chanhead==NULL) {
      ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "can't allocate memory for (new) channel subscriber head");
      return NGX_ERROR;
    }
    chan_id=(ngx_str_t *)(chanhead+1);
    chanhead->id=chan_id;

    chan_id->data=(u_char *)(chan_id+1);
    chan_id->len=channel_id->len;
    ngx_memcpy(chan_id->data, channel_id->data, channel_id->len);

    chanhead->pool=NULL;
    chanhead->sub=NULL;
    chanhead->sub_count=0;
    chanhead->cleanlink=NULL;
    CHANNEL_HASH_ADD(chanhead);
  }
  if(chanhead_ensure_pool_exists(chanhead)==NULL) {
    return NGX_ERROR;
  }
  
  if((nextsub=ngx_palloc(chanhead->pool, sizeof(*nextsub)))==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "can't allocate memory for (new) subscriber in channel sub pool");
    return NGX_ERROR;
  }
  
  nextsub->subscriber=(void *)r;
  nextsub->type= LONGPOLL;
  nextsub->pool= r->pool;
  nextsub->next= chanhead->sub;
  chanhead->sub= nextsub;
  chanhead->sub_count++;
  
  //remove from cleanup list if we're there
  if(chanhead->cleanlink!=NULL) {
    nhpm_llist_timed_t    *cl=chanhead->cleanlink;
    if(cl->prev!=NULL)
      cl->prev->next=cl->next;
    if(cl->next!=NULL)
      cl->next->prev=cl->prev;
    if(chanhead_cleanup_head==cl)
      chanhead_cleanup_head=cl->next;
    if(chanhead_cleanup_tail==cl)
      chanhead_cleanup_tail=cl->prev;
  }

  redisAsyncCommand(rds_ctx(), &redisEchoCallback, NULL, "EVALSHA %s 0 %b %i", nhpm_rds_lua_hashes.subscriber_count, STR(channel_id), 1);

  ngx_push_longpoll_subscriber_enqueue(nextsub->subscriber, cf->subscriber_timeout);
  return NGX_OK;
}

static ngx_int_t default_subscribe_callback(ngx_int_t status, ngx_http_request_t *r) {
  return status;
}

typedef struct {
  ngx_str_t              *channel_id;
  ngx_http_push_msg_id_t *msg_id;
  ngx_http_request_t     *r;
  ngx_int_t             (*callback)(ngx_int_t status, ngx_http_request_t *r);
} redis_subscribe_data_t;

static void redis_subscribe_callback(redisAsyncContext *c, void *vr, void *privdata) {
  redis_subscribe_data_t    *d = (redis_subscribe_data_t *) privdata;
  redisReply                *reply = (redisReply *)vr;
  ngx_http_push_loc_conf_t  *cf = ngx_http_get_module_loc_conf(d->r, ngx_http_push_module);
  ngx_int_t                  status;
  //output: result_code, msg_time, msg_tag, message, content_type, channel_subscriber_count
  // result_code can be: 200 - ok, 403 - channel not found, 404 - not found, 410 - gone, 418 - not yet available
  
  if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
    redisEchoCallback(c,reply,privdata);
    ngx_free(d);
    return;
  }
  
  if ( !CHECK_REPLY_ARRAY_MIN_SIZE(reply, 1) || !CHECK_REPLY_INT(reply->element[0]) ) {
    //no good
    ngx_free(d);
    return;
  }
  
  status = reply->element[0]->integer;
  
  switch(status) {
    ngx_str_t                  *etag;
    ngx_str_t                  *content_type;
    ngx_chain_t                *chain=NULL;
    time_t                      last_modified;
    ngx_int_t                   ret;
    ngx_http_push_msg_t        *msg=NULL;
    
    case 200: //ok
      switch(cf->subscriber_concurrency) {
        case NGX_HTTP_PUSH_SUBSCRIBER_CONCURRENCY_BROADCAST:
          if((msg=msg_from_get_message_reply(reply, 1, &ngx_nhpm_alloc))) {
            ngx_http_push_alloc_for_subscriber_response(d->r->pool, 0, msg, &chain, &content_type, &etag, &last_modified);
            ret=ngx_http_push_prepare_response_to_subscriber_request(d->r, chain, content_type, etag, last_modified);
            d->callback(ret, d->r);
          }
          break;

        case NGX_HTTP_PUSH_SUBSCRIBER_CONCURRENCY_LASTIN:
          //kick everyone elese out, then subscribe
          redisAsyncCommand(rds_ctx(), &redisEchoCallback, NULL, "EVALSHA %s 0 %b %i", nhpm_rds_lua_hashes.publish_status, STR(d->channel_id), 409);
          if((msg=msg_from_get_message_reply(reply, 1, &ngx_nhpm_alloc))) {
            ngx_http_push_alloc_for_subscriber_response(d->r->pool, 0, msg, &chain, &content_type, &etag, &last_modified);
            ret=ngx_http_push_prepare_response_to_subscriber_request(d->r, chain, content_type, etag, last_modified);
            d->callback(ret, d->r);
          }
          break;

        case NGX_HTTP_PUSH_SUBSCRIBER_CONCURRENCY_FIRSTIN:
          if(CHECK_REPLY_ARRAY_MIN_SIZE(reply, 6) && CHECK_REPLY_INT(reply->element[5]) && reply->element[5]->integer > 0 ) {
            ngx_http_push_respond_status_only(d->r, NGX_HTTP_NOT_FOUND, &NGX_HTTP_PUSH_HTTP_STATUS_409);
          }
          break;

        default:
          ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "unexpected subscriber_concurrency config value");
      }
      break;
    case 418: //not yet available
    case 403: //channel not found
      // ♫ It's gonna be the future soon ♫
      if (ngx_http_push_store_subscribe_new(d->channel_id, d->r) == NGX_OK) {
        d->callback(NGX_DONE, d->r);
      }
      else {
        d->callback(NGX_ERROR, d->r);
      }
      break;
    case 404: //not found
      d->callback(NGX_HTTP_NOT_FOUND, d->r);
      break;
    case 410: //gone
      //subscriber wants an expired message
      //TODO: maybe respond with entity-identifiers for oldest available message?
      d->callback(NGX_HTTP_NO_CONTENT, d->r);
      break;
    default: //shouldn't be here!
      d->callback(NGX_HTTP_INTERNAL_SERVER_ERROR, d->r);
  }
  
  ngx_free(d);
}

static ngx_int_t ngx_http_push_store_subscribe(ngx_str_t *channel_id, ngx_http_push_msg_id_t *msg_id, ngx_http_request_t *r, ngx_int_t (*callback)(ngx_int_t status, ngx_http_request_t *r)) {
  redis_subscribe_data_t       *d=NULL;
  ngx_http_push_loc_conf_t     *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_module);
  //static ngx_int_t                       subscribed = 0;
  
  if(callback == NULL) {
    callback=&default_subscribe_callback;
  }
  
  
  if((d=ngx_alloc(sizeof(*d), ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "can't allocate redis get_message callback data");
    return callback(NGX_HTTP_INTERNAL_SERVER_ERROR, r);
  }
  d->r=r;
  d->channel_id=channel_id;
  d->msg_id=msg_id;
  d->callback=callback;
  
  //input:  keys: [], values: [channel_id, msg_time, msg_tag, no_msgid_order, create_channel_ttl, subscriber_channel]
  redisAsyncCommand(rds_ctx(), &redis_subscribe_callback, (void *)d, "EVALSHA %s 0 %b %i %i %s %i %s", nhpm_rds_lua_hashes.get_message, STR(channel_id), msg_id->time, msg_id->tag, "FILO", cf->channel_timeout, subscriber_channel);
  return NGX_OK;
}
 /* 
  ngx_http_push_msg_t            *msg;
  ngx_int_t                       msg_search_outcome;
  ngx_http_push_loc_conf_t       *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_module);
  if (cf->authorize_channel==1) {
    channel = ngx_http_push_store_find_channel(channel_id, cf->channel_timeout, NULL);
  }else{
    channel = ngx_http_push_store_get_channel(channel_id, cf->channel_timeout, NULL);
  }
  if (channel==NULL) {
    //unable to allocate channel OR channel not found
    if(cf->authorize_channel) {
      return callback(NGX_HTTP_FORBIDDEN, r);
    }
    else {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: unable to allocate shared memory for channel");
      return callback(NGX_HTTP_INTERNAL_SERVER_ERROR, r);
    }
  }
  
  switch(ngx_http_push_handle_subscriber_concurrency(channel, r, cf)) {
    case NGX_DECLINED: //this request was declined for some reason.
      //status codes and whatnot should have already been written. just get out of here quickly.
      return callback(NGX_OK, r);
    case NGX_ERROR:
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: error handling subscriber concurrency setting");
      return callback(NGX_ERROR, r);
  }
  
  
  msg = ngx_http_push_store_get_channel_message(channel, msg_id, &msg_search_outcome, cf);
  
  if (cf->ignore_queue_on_no_cache && !ngx_http_push_allow_caching(r)) {
    msg_search_outcome = NGX_HTTP_PUSH_MESSAGE_EXPECTED; 
    msg = NULL;
  }
  
  switch(msg_search_outcome) {
    //for message-found:
    ngx_str_t                  *etag;
    ngx_str_t                  *content_type;
    ngx_chain_t                *chain=NULL;
    time_t                      last_modified;

    case NGX_HTTP_PUSH_MESSAGE_EXPECTED:
      // ♫ It's gonna be the future soon ♫
      if (ngx_http_push_store_subscribe_new(channel, r) == NGX_OK) {

        redisAsyncCommand(rds_ctx(), NULL, NULL, "ECHO SUB channel:%b", STR(&(channel->id)));
        return callback(NGX_DONE, r);
      }
      else {
        return callback(NGX_ERROR, r);
      }

    case NGX_HTTP_PUSH_MESSAGE_EXPIRED:
      //subscriber wants an expired message
      //TODO: maybe respond with entity-identifiers for oldest available message?
      return callback(NGX_HTTP_NO_CONTENT, r);
      
    case NGX_HTTP_PUSH_MESSAGE_FOUND:
      ngx_http_push_alloc_for_subscriber_response(r->pool, 0, msg, &chain, &content_type, &etag, &last_modified);
      ngx_int_t ret=ngx_http_push_prepare_response_to_subscriber_request(r, chain, content_type, etag, last_modified);
      ngx_http_push_store->release_message(channel, msg);
      return callback(ret, r);
      
    default: //we shouldn't be here.
      return callback(NGX_HTTP_INTERNAL_SERVER_ERROR, r);
  }
*/

static ngx_str_t * ngx_http_push_store_etag_from_message(ngx_http_push_msg_t *msg, ngx_pool_t *pool){
  ngx_str_t *etag;
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  if(pool!=NULL && (etag = ngx_palloc(pool, sizeof(*etag) + NGX_INT_T_LEN))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: unable to allocate memory for Etag header in pool");
    return NULL;
  }
  else if(pool==NULL && (etag = ngx_alloc(sizeof(*etag) + NGX_INT_T_LEN, ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: unable to allocate memory for Etag header");
    return NULL;
  }
  etag->data = (u_char *)(etag+1);
  etag->len = ngx_sprintf(etag->data,"%ui", msg->message_tag)- etag->data;
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  return etag;
}

static ngx_str_t * ngx_http_push_store_content_type_from_message(ngx_http_push_msg_t *msg, ngx_pool_t *pool){
  ngx_str_t *content_type;
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  if(pool != NULL && (content_type = ngx_palloc(pool, sizeof(*content_type) + msg->content_type.len))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: unable to allocate memory for Content Type header in pool");
    return NULL;
  }
  else if(pool == NULL && (content_type = ngx_alloc(sizeof(*content_type) + msg->content_type.len, ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: unable to allocate memory for Content Type header");
    return NULL;
  }
  content_type->data = (u_char *)(content_type+1);
  content_type->len = msg->content_type.len;
  ngx_memcpy(content_type->data, msg->content_type.data, content_type->len);
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  return content_type;
}

// this function adapted from push stream module. thanks Wandenberg Peixoto <wandenberg@gmail.com> and Rogério Carvalho Schneider <stockrt@gmail.com>
static ngx_buf_t * ngx_http_push_request_body_to_single_buffer(ngx_http_request_t *r) {
  ngx_buf_t *buf = NULL;
  ngx_chain_t *chain;
  ssize_t n;
  off_t len;

  chain = r->request_body->bufs;
  if (chain->next == NULL) {
    return chain->buf;
  }
  if (chain->buf->in_file) {
    if (ngx_buf_in_memory(chain->buf)) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: can't handle a buffer in a temp file and in memory ");
    }
    if (chain->next != NULL) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: error reading request body with multiple ");
    }
    return chain->buf;
  }
  buf = ngx_create_temp_buf(r->pool, r->headers_in.content_length_n + 1);
  if (buf != NULL) {
    ngx_memset(buf->start, '\0', r->headers_in.content_length_n + 1);
    while ((chain != NULL) && (chain->buf != NULL)) {
      len = ngx_buf_size(chain->buf);
      // if buffer is equal to content length all the content is in this buffer
      if (len >= r->headers_in.content_length_n) {
        buf->start = buf->pos;
        buf->last = buf->pos;
        len = r->headers_in.content_length_n;
      }
      if (chain->buf->in_file) {
        n = ngx_read_file(chain->buf->file, buf->start, len, 0);
        if (n == NGX_FILE_ERROR) {
          ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: cannot read file with request body");
          return NULL;
        }
        buf->last = buf->last + len;
        ngx_delete_file(chain->buf->file->name.data);
        chain->buf->file->fd = NGX_INVALID_FILE;
      } else {
        buf->last = ngx_copy(buf->start, chain->buf->pos, len);
      }

      chain = chain->next;
      buf->start = buf->last;
    }
  }
  return buf;
}



#define NGX_HTTP_PUSH_BROADCAST_CHECK(val, fail, r, errormessage)             \
    if (val == fail) {                                                        \
        ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, errormessage);    \
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);         \
        return NULL;                                                          \
  }

#define NGX_HTTP_PUSH_BROADCAST_CHECK_LOCKED(val, fail, r, errormessage, shpool) \
    if (val == fail) {                                                        \
        ngx_shmtx_unlock(&(shpool)->mutex);                                   \
        ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, errormessage);    \
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);         \
        return NULL;                                                          \
    }

#define NGX_HTTP_BUF_ALLOC_SIZE(buf)                                          \
    (sizeof(*buf) +                                                           \
   (((buf)->temporary || (buf)->memory) ? ngx_buf_size(buf) : 0) +          \
   (((buf)->file!=NULL) ? (sizeof(*(buf)->file) + (buf)->file->name.len + 1) : 0))


static ngx_http_push_msg_t * ngx_http_push_store_create_message(ngx_http_push_channel_t *channel, ngx_http_request_t *r) {
  ngx_buf_t                      *buf = NULL, *buf_copy;
  size_t                          content_type_len;
  ngx_http_push_loc_conf_t       *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_module);
  ngx_http_push_msg_t            *msg, *previous_msg;
  
  //first off, we'll want to extract the body buffer
  
  //note: this works mostly because of r->request_body_in_single_buf = 1; 
  //which, i suppose, makes this module a little slower than it could be.
  //this block is a little hacky. might be a thorn for forward-compatibility.
  if(r->headers_in.content_length_n == -1 || r->headers_in.content_length_n == 0) {
    buf = ngx_create_temp_buf(r->pool, 0);
    //this buffer will get copied to shared memory in a few lines, 
    //so it does't matter what pool we make it in.
  }
  else if(r->request_body->bufs!=NULL) {
    buf = ngx_http_push_request_body_to_single_buffer(r);
  }
  else {
    ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, "push module: unexpected publisher message request body buffer location. please report this to the push module developers.");
    return NULL;
  }
  
  NGX_HTTP_PUSH_BROADCAST_CHECK(buf, NULL, r, "push module: can't find or allocate publisher request body buffer");
      
  content_type_len = (r->headers_in.content_type!=NULL ? r->headers_in.content_type->value.len : 0);
  
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  
  //create a buffer copy in shared mem
  msg = ngx_http_push_slab_alloc_locked(sizeof(*msg) + content_type_len, "message + content_type");
  NGX_HTTP_PUSH_BROADCAST_CHECK_LOCKED(msg, NULL, r, "push module: unable to allocate message in shared memory", ngx_http_push_shpool);
  previous_msg=ngx_http_push_get_latest_message_locked(channel); //need this for entity-tags generation
  
  buf_copy = ngx_http_push_slab_alloc_locked(NGX_HTTP_BUF_ALLOC_SIZE(buf), "message buffer copy");
  NGX_HTTP_PUSH_BROADCAST_CHECK_LOCKED(buf_copy, NULL, r, "push module: unable to allocate buffer in shared memory", ngx_http_push_shpool) //magic nullcheck
  ngx_http_push_copy_preallocated_buffer(buf, buf_copy);
  
  msg->buf=buf_copy;
  
  //Stamp the new message with entity tags
  msg->message_time=ngx_time(); //ESSENTIAL TODO: make sure this ends up producing GMT time
  msg->message_tag=(previous_msg!=NULL && msg->message_time == previous_msg->message_time) ? (previous_msg->message_tag + 1) : 0;    
  
  //store the content-type
  if(content_type_len>0) {
    msg->content_type.len=r->headers_in.content_type->value.len;
    msg->content_type.data=(u_char *) (msg+1); //we had reserved a contiguous chunk, myes?
    ngx_memcpy(msg->content_type.data, r->headers_in.content_type->value.data, msg->content_type.len);
  }
  else {
    msg->content_type.len=0;
    msg->content_type.data=NULL;
  }
  
  //queue stuff ought to be NULL
  msg->queue.prev=NULL;
  msg->queue.next=NULL;
  
  msg->refcount=0;
  
  //set message expiration time
  time_t                  message_timeout = cf->buffer_timeout;
  msg->expires = (message_timeout==0 ? 0 : (ngx_time() + message_timeout));
  
  msg->delete_oldest_received_min_messages = cf->delete_oldest_received_message ? (ngx_uint_t) cf->min_messages : NGX_MAX_UINT32_VALUE;
  //NGX_MAX_UINT32_VALUE to disable, otherwise = min_message_buffer_size of the publisher location from whence the message came
  
  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, CREATED_DBG, msg, msg->refcount, msg->queue.prev, msg->queue.next);
  return msg;
}

static ngx_int_t ngx_http_push_store_enqueue_message(ngx_http_push_channel_t *channel, ngx_http_push_msg_t *msg, ngx_http_push_loc_conf_t *cf) {
  ngx_shmtx_lock(&ngx_http_push_shpool->mutex);
  ngx_queue_insert_tail(&channel->message_queue->queue, &msg->queue);
  channel->messages++;
  
  //now see if the queue is too big
  if(channel->messages > (ngx_uint_t) cf->max_messages) {
    //exceeeds max queue size. don't force it, someone might still be using this message.
    ngx_http_push_delete_message_locked(channel, ngx_http_push_get_oldest_message_locked(channel), 0);
  }
  if(channel->messages > (ngx_uint_t) cf->min_messages) {
    //exceeeds min queue size. maybe delete the oldest message
    //no, don't do anything for now. This feature is badly implemented and I think I'll deprecate it.
  }

  ngx_shmtx_unlock(&ngx_http_push_shpool->mutex);
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, ENQUEUED_DBG, msg, msg->refcount, msg->queue.prev, msg->queue.next);
  return NGX_OK;
}


static ngx_int_t default_publish_callback(ngx_int_t status, ngx_http_push_channel_t *ch, ngx_http_request_t *r) {
  return status;
}




typedef struct {
  ngx_str_t            *channel_id;
  time_t                msg_time;
  ngx_http_request_t   *r;
  ngx_http_push_msg_t  *msg;
} redis_publish_callback_data_t;

static void redisPublishCallback(redisAsyncContext *c, void *r, void *privdata);

static ngx_int_t ngx_http_push_store_publish_message(ngx_str_t *channel_id, ngx_http_request_t *r, ngx_int_t (*callback)(ngx_int_t status, ngx_http_push_channel_t *ch, ngx_http_request_t *r)) {
  ngx_http_push_channel_t        *channel;
  ngx_http_push_msg_t            *msg;
  ngx_http_push_loc_conf_t       *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_module);
  ngx_int_t                       result=0;
  
  redis_publish_callback_data_t *d=NULL;
  
  if(callback==NULL) {
    callback=&default_publish_callback;
  }
  
  if((channel=ngx_http_push_store_get_channel(channel_id, cf->channel_timeout, NULL))==NULL) { //always returns a channel, unless no memory left
    return callback(NGX_ERROR, NULL, r);
    //ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
  }
  
  if((msg = ngx_http_push_store_create_message(channel, r))==NULL) {
    return callback(NGX_ERROR, channel, r);
    //ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
  }
  
  if(cf->max_messages > 0) { //channel buffers exist
    ngx_http_push_store_enqueue_message(channel, msg, cf);
  }
  else if(cf->max_messages == 0) {
    ngx_http_push_store_reserve_message(NULL, msg);
  }
  result= ngx_http_push_store_publish_raw(channel_id, msg, 0, NULL);

  if((d=ngx_alloc(sizeof(*d), ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "can't allocate redis publish callback data");
    return callback(NGX_ERROR, channel, r);
  }

  d->channel_id=channel_id;
  d->msg_time=ngx_time();
  d->msg=msg;
  d->r=r;

  //input:  keys: [], values: [channel_id, time, message, content_type, msg_ttl]
  //output: message_tag, channel_hash
  redisAsyncCommand(rds_ctx(), &redisPublishCallback, (void *)d, "EVALSHA %s 0 %b %i %b %b %i", nhpm_rds_lua_hashes.publish, STR(channel_id), d->msg_time, BUF(msg->buf), STR(&(msg->content_type)), cf->buffer_timeout);
  return callback(result, channel, r);
}

static void redisPublishCallback(redisAsyncContext *c, void *r, void *privdata) {
  redis_publish_callback_data_t *d=(redis_publish_callback_data_t *)privdata;
  redisReply *reply=r;
  redisReply *cur;
  ngx_http_push_msg_id_t msg_id;
  
  if (reply->type != REDIS_REPLY_ARRAY) {
    return;
  }
  
  if (reply->elements < 2) {
    return;
  }
  
  msg_id.time=d->msg_time;
  msg_id.tag=reply->element[0]->integer;
  
  cur=reply->element[1];
  if (cur->type == REDIS_REPLY_ARRAY) {
    //channel info
    ngx_int_t ttl = cur->element[0]->integer;
    time_t last_seen = cur->element[1]->integer;
    ngx_int_t subscribers = cur->element[2]->integer;
    //do some stuff
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "ttl:%i last_seen:%i subscribers:%i, mdssg id %i:%i", ttl, last_seen, subscribers, msg_id.time, msg_id.tag);
  }
  ngx_free(d);
}

static ngx_int_t ngx_http_push_store_send_worker_message(ngx_http_push_channel_t *channel, ngx_http_push_subscriber_t *subscriber_sentinel, ngx_pid_t pid, ngx_int_t worker_slot, ngx_http_push_msg_t *msg, ngx_int_t status_code) {
  ngx_http_push_worker_msg_sentinel_t   *worker_messages = ((ngx_http_push_shm_data_t *)ngx_http_push_shm_zone->data)->ipc;
  ngx_http_push_worker_msg_sentinel_t   *sentinel = &worker_messages[worker_slot];
  ngx_http_push_worker_msg_t            *newmessage;
  
  if((newmessage=ngx_http_push_slab_alloc(sizeof(*newmessage), "IPC worker message"))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: unable to allocate worker message");
    return NGX_ERROR;
  }
  newmessage->msg = msg;
  newmessage->status_code = status_code;
  newmessage->pid = pid;
  newmessage->subscriber_sentinel = subscriber_sentinel;
  newmessage->channel = channel;
  
  ngx_http_push_store_lock_shmem();
  ngx_queue_insert_tail(&sentinel->queue, &newmessage->queue);
  ngx_http_push_store_unlock_shmem();
  return NGX_OK;
  
}

static void ngx_http_push_store_receive_worker_message(void) {
  ngx_http_push_worker_msg_t     *prev_worker_msg, *worker_msg;
  ngx_http_push_worker_msg_sentinel_t    *sentinel;
  const ngx_str_t                *status_line = NULL;
  ngx_http_push_channel_t        *channel;
  ngx_http_push_subscriber_t     *subscriber_sentinel;
  ngx_int_t                       worker_msg_pid;
  
  ngx_int_t                       status_code;
  ngx_http_push_msg_t            *msg;
  
  sentinel = &(((ngx_http_push_shm_data_t *)ngx_http_push_shm_zone->data)->ipc)[ngx_process_slot];
  
  ngx_http_push_store_lock_shmem();
  worker_msg = (ngx_http_push_worker_msg_t *)ngx_queue_next(&sentinel->queue);
  ngx_http_push_store_unlock_shmem();
  while((void *)worker_msg != (void *)sentinel) {
    
    ngx_http_push_store_lock_shmem();
    worker_msg_pid = worker_msg->pid;
    ngx_http_push_store_unlock_shmem();
    
    if(worker_msg_pid == ngx_pid) {
      //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "process_worker_message processing proper worker_msg ");
      //everything is okay.
      
      ngx_http_push_store_lock_shmem();
      status_code = worker_msg->status_code;
      msg = worker_msg->msg;
      channel = worker_msg->channel;
      subscriber_sentinel = worker_msg->subscriber_sentinel;
      ngx_http_push_store_unlock_shmem();
      
      if(msg==NULL) {
        //just a status line, is all    
        //status code only.
        switch(status_code) {
          case NGX_HTTP_CONFLICT:
            status_line=&NGX_HTTP_PUSH_HTTP_STATUS_409;
            break;
            
          case NGX_HTTP_GONE:
            status_line=&NGX_HTTP_PUSH_HTTP_STATUS_410;
            break;
            
          case 0:
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: worker message contains neither a channel message nor a status code");
            //let's let the subscribers know that something went wrong and they might've missed a message
            status_code = NGX_HTTP_INTERNAL_SERVER_ERROR; 
            //intentional fall-through
          default:
            status_line=NULL;
        }
      }

      ngx_http_push_respond_to_subscribers(channel, subscriber_sentinel, msg, status_code, status_line);

    }
    else {
      //that's quite bad you see. a previous worker died with an undelivered message.
      //but all its subscribers' connections presumably got canned, too. so it's not so bad after all.
      //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "process_worker_message processing INVALID worker_msg ");
      
      ngx_http_push_store_lock_shmem();
      
      ngx_http_push_pid_queue_t     *channel_worker_sentinel = worker_msg->channel->workers_with_subscribers;
      
      ngx_http_push_pid_queue_t     *channel_worker_cur = channel_worker_sentinel;
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "push module: worker %i intercepted a message intended for another worker process (%i) that probably died", ngx_pid, worker_msg->pid);
      
      //delete that invalid sucker.
      while((channel_worker_cur=(ngx_http_push_pid_queue_t *)ngx_queue_next(&channel_worker_cur->queue))!=channel_worker_sentinel) {
        if(channel_worker_cur->pid == worker_msg->pid) {
          ngx_queue_remove(&channel_worker_cur->queue);
          ngx_http_push_slab_free_locked(channel_worker_cur);
          break;
        }
      }
      
      ngx_http_push_store_unlock_shmem();
      
    }
    //It may be worth it to memzero worker_msg for debugging purposes.
    prev_worker_msg = worker_msg;
    
    ngx_http_push_store_lock_shmem();
    worker_msg = (ngx_http_push_worker_msg_t *)ngx_queue_next(&worker_msg->queue);
    ngx_http_push_slab_free_locked(prev_worker_msg);
    ngx_http_push_store_unlock_shmem();

  }
  ngx_http_push_store_lock_shmem();
  ngx_queue_init(&sentinel->queue); //reset the worker message sentinel
  ngx_http_push_store_unlock_shmem();
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "process_worker_message finished");
  return;
}

ngx_http_push_store_t  ngx_http_push_store_redis = {
    //init
    &ngx_http_push_store_init_module,
    &ngx_http_push_store_init_worker,
    &ngx_http_push_store_init_postconfig,
    &ngx_http_push_store_create_main_conf,
    
    //shutdown
    &ngx_http_push_store_exit_worker,
    &ngx_http_push_store_exit_master,
    
    //async-friendly functions with callbacks
    &ngx_http_push_store_async_get_message, //+callback
    &ngx_http_push_store_subscribe, //+callback
    &ngx_http_push_store_publish_message, //+callback
    
    //channel stuff,
    &ngx_http_push_store_get_channel, //creates channel if not found, +callback
    &ngx_http_push_store_find_channel, //returns channel or NULL if not found, +callback
    &ngx_http_push_store_delete_channel,
    
    &ngx_http_push_store_get_channel_message,
    &ngx_http_push_store_reserve_message,
    &ngx_http_push_store_release_message,
    
    //channel properties
    &ngx_http_push_store_channel_subscribers,
    &ngx_http_push_store_channel_worker_subscribers,
    &ngx_http_push_store_channel_next_subscriber,
    &ngx_http_push_store_channel_release_subscriber_sentinel,

    //legacy shared-memory store helpers
    &ngx_http_push_store_lock_shmem,
    &ngx_http_push_store_unlock_shmem,
    &ngx_http_push_slab_alloc_locked,
    &ngx_http_push_slab_free_locked,
    
    //message stuff
    &ngx_http_push_store_create_message,
    &ngx_http_push_delete_message,
    &ngx_http_push_delete_message_locked,
    &ngx_http_push_store_enqueue_message,
    &ngx_http_push_store_etag_from_message,
    &ngx_http_push_store_content_type_from_message,
    
    //interprocess communication
    &ngx_http_push_store_send_worker_message,
    &ngx_http_push_store_receive_worker_message
    

};