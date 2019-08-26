#ifdef HAVE_CONFIG_H
# include "elementary_config.h"
#endif

#define ELM_LAYOUT_PROTECTED
#define EFL_UI_SCROLL_MANAGER_PROTECTED
#define EFL_UI_SCROLLBAR_PROTECTED
#define EFL_UI_WIDGET_FOCUS_MANAGER_PROTECTED

#include <Efl_Ui.h>
#include <Elementary.h>
#include "elm_widget.h"
#include "elm_priv.h"

#include "efl_ui_collection_view_focus_manager.eo.h"

typedef struct _Efl_Ui_Collection_View_Data Efl_Ui_Collection_View_Data;
typedef struct _Efl_Ui_Collection_Viewport Efl_Ui_Collection_Viewport;
typedef struct _Efl_Ui_Collection_View_Focus_Manager_Data Efl_Ui_Collection_View_Focus_Manager_Data;
typedef struct _Efl_Ui_Collection_Item Efl_Ui_Collection_Item;
typedef struct _Efl_Ui_Collection_Item_Lookup Efl_Ui_Collection_Item_Lookup;
typedef struct _Efl_Ui_Collection_Request Efl_Ui_Collection_Request;

struct _Efl_Ui_Collection_Item
{
   Efl_Gfx_Entity *entity;
   Efl_Model *model;
};

struct _Efl_Ui_Collection_Item_Lookup
{
   EINA_RBTREE;

   uint64_t index;
   Efl_Ui_Collection_Item item;
};

struct _Efl_Ui_Collection_Viewport
{
   Efl_Ui_Collection_Item *items;

   uint64_t offset;
   uint16_t count;
};

struct _Efl_Ui_Collection_Request
{
   Eina_Future *f;

   uint64_t offset;
   uint64_t length;

   Eina_Bool model_requested : 1;
   Eina_Bool entity_requested : 1;
};

struct _Efl_Ui_Collection_View_Data
{
   Efl_Ui_Factory *factory;
   Efl_Ui_Position_Manager_Entity *manager;
   Efl_Ui_Scroll_Manager *scroller;
   Efl_Ui_Pan *pan;
   Efl_Gfx_Entity *sizer;
   Efl_Model *model;

   Efl_Ui_Collection_Viewport *viewport[3];
   Eina_Rbtree *cache;

   Eina_List *requests; // Array of Efl_Ui_Collection_Request in progress

   uint64_t start_id;
   uint64_t end_id;

   Eina_Size2D last_base;
   Eina_Size2D content_min_size;

   Efl_Ui_Layout_Orientation direction;
   Efl_Ui_Select_Mode mode;

   struct {
      Eina_Bool w : 1;
      Eina_Bool h : 1;
   } match_content;
};

struct _Efl_Ui_Collection_View_Focus_Manager_Data
{
   Efl_Ui_Collection_View *collection;
};

static const char *COLLECTION_VIEW_MANAGED = "_collection_view.managed";
static const char *COLLECTION_VIEW_MANAGED_YES = "yes";

#define MY_CLASS EFL_UI_COLLECTION_VIEW_CLASS

#define MY_DATA_GET(obj, pd)                                            \
  Efl_Ui_Collection_View_Data *pd = efl_data_scope_get(obj, MY_CLASS);

static int
_cache_tree_lookup(const Eina_Rbtree *node, const void *key,
                   int length EINA_UNUSED, void *data EINA_UNUSED)
{
   const Efl_Ui_Collection_Item_Lookup *n = (Efl_Ui_Collection_Item_Lookup *)node;
   const uint64_t *index = key;

   return n->index - *index;
}

static Eina_Rbtree_Direction
_cache_tree_cmp(const Eina_Rbtree *left, const Eina_Rbtree *right, void *data EINA_UNUSED)
{
   Efl_Ui_Collection_Item_Lookup *l = (Efl_Ui_Collection_Item_Lookup *)left;
   Efl_Ui_Collection_Item_Lookup *r = (Efl_Ui_Collection_Item_Lookup *)right;

   return l->index < r->index ? EINA_RBTREE_LEFT : EINA_RBTREE_RIGHT;
}

static void
_item_cleanup(Efl_Ui_Factory *factory, Efl_Ui_Collection_Item *item)
{
   Efl_Gfx_Entity *entity;

   efl_replace(&item->model, NULL);
   entity = item->entity;
   if (!entity) return ;

   efl_ui_view_model_set(entity, NULL);
   efl_replace(&item->entity, NULL);
   efl_ui_factory_release(factory, entity);
}

static void
_cache_item_free(Eina_Rbtree *node, void *data)
{
   Efl_Ui_Collection_Item_Lookup *n = (void*) node;
   Efl_Ui_Collection_View_Data *pd = data;

   _item_cleanup(pd->factory, &n->item);
   free(n);
}

static void
_cache_cleanup(Efl_Ui_Collection_View_Data *pd)
{
   eina_rbtree_delete(pd->cache, _cache_item_free, pd);
}

static void
_all_cleanup(Efl_Ui_Collection_View_Data *pd)
{
   Efl_Ui_Collection_Request *request;
   Eina_List *l, *ll;
   unsigned int i;

   _cache_cleanup(pd);
   for (i = 0; i < 3; i++)
     {
        unsigned int j;

        if (!pd->viewport[i]) continue;

        for (j = 0; j < pd->viewport[i]->count; j++)
          _item_cleanup(pd->factory, &(pd->viewport[i]->items[j]));
     }

   EINA_LIST_FOREACH_SAFE(pd->requests, l, ll, request)
     eina_future_cancel(request->f);
}

static inline Eina_Bool
_size_from_model(Efl_Model *model, Eina_Size2D *r, const char *width, const char *height)
{
   Eina_Value *vw, *vh;
   Eina_Bool success = EINA_FALSE;

   EINA_SAFETY_ON_NULL_RETURN_VAL(model, EINA_FALSE);

   vw = efl_model_property_get(model, width);
   vh = efl_model_property_get(model, height);

   if (eina_value_type_get(vw) == EINA_VALUE_TYPE_ERROR ||
       eina_value_type_get(vh) == EINA_VALUE_TYPE_ERROR)
     goto on_error;

   if (!eina_value_int_convert(vw, &(r->w))) r->w = 0;
   if (!eina_value_int_convert(vh, &(r->h))) r->h = 0;

   success = EINA_TRUE;

 on_error:
   eina_value_free(vw);
   eina_value_free(vh);

   return success;
}

static inline void
_size_to_model(Efl_Model *model, Eina_Size2D state)
{
   Eina_Value vw, vh;

   vw = eina_value_int_init(state.w);
   vh = eina_value_int_init(state.h);

   efl_model_property_set(model, "self.width", &vw);
   efl_model_property_set(model, "selft.height", &vh);

   eina_value_flush(&vw);
   eina_value_flush(&vh);
}

#define ITEM_BASE_SIZE_FROM_MODEL(Model, Size) _size_from_model(Model, &Size, "item.width", "item.height")
#define ITEM_SIZE_FROM_MODEL(Model, Size) _size_from_model(Model, &Size, "self.width", "self.height")

static Eina_List *
_request_add(Eina_List *requests, Efl_Ui_Collection_Request **request,
             uint64_t index, Eina_Bool need_entity)
{
   if (!(*request)) goto create;

   if ((*request)->offset + (*request)->length == index)
     {
        if (need_entity) (*request)->entity_requested = EINA_TRUE;
        (*request)->length += 1;
        return requests;
     }

   requests = eina_list_append(requests, request);

 create:
   *request = calloc(1, sizeof (Efl_Ui_Collection_Request));
   if (!(*request)) return requests;
   (*request)->offset = index;
   (*request)->length = 1;
   // At this point, we rely on the model caching ability to avoid recreating model
   (*request)->model_requested = EINA_TRUE;
   (*request)->entity_requested = !!need_entity;

   return requests;
}

static Eina_Value
_model_fetched_cb(Eo *obj, void *data, const Eina_Value v)
{
   MY_DATA_GET(obj, pd);
   Efl_Ui_Collection_Request *request = data;
   Efl_Model *child;
   unsigned int i, len;

   EINA_VALUE_ARRAY_FOREACH(&v, len, i, child)
     {
        Efl_Ui_Collection_Item_Lookup *insert;
        unsigned int v;

        for (v = 0; v < 3; ++v)
          {
             if (!pd->viewport[v]) continue;

             if ((pd->viewport[v]->offset <= request->offset + i) &&
                 (request->offset + i < pd->viewport[v]->offset + pd->viewport[v]->count))
               {
                  uint64_t index = request->offset + i - pd->viewport[v]->offset;

                  efl_replace(&pd->viewport[v]->items[index].model, child);
                  child = NULL;
                  break;
               }
          }

        // When requesting a model, it should not be in the cache prior to the request
        if (!child) continue;

        insert = calloc(1, sizeof (Efl_Ui_Collection_Item_Lookup));
        if (!insert) continue;

        insert->index = request->offset + i;
        insert->item.model = efl_ref(child);

        pd->cache = eina_rbtree_inline_insert(pd->cache, EINA_RBTREE_GET(insert), _cache_tree_cmp, NULL);
     }

   return v;
}

static void
_model_free_cb(Eo *o, void *data, const Eina_Future *dead_future EINA_UNUSED)
{
   MY_DATA_GET(o, pd);
   Efl_Ui_Collection_Request *request = data;

   if (request->entity_requested) return;
   pd->requests = eina_list_remove(pd->requests, request);
   free(request);
}

static Eina_Value
_entity_fetch_cb(Eo *obj, void *data EINA_UNUSED, const Eina_Value v)
{
   MY_DATA_GET(obj, pd);
   Efl_Model *child;
   Eina_Future *r;
   Eina_Array tmp;
   unsigned int i, len;

   eina_array_step_set(&tmp, sizeof (Eina_Array), 4);

   EINA_VALUE_ARRAY_FOREACH(&v, len, i, child)
     {
        eina_array_push(&tmp, child);
     }

   r = efl_ui_view_factory_create_with_event(pd->factory, eina_array_iterator_new(&tmp), obj);

   eina_array_flush(&tmp);

   return eina_future_as_value(r);
}

static inline Eina_Bool
_entity_propagate(Efl_Model *model, Efl_Gfx_Entity *entity)
{
   Eina_Size2D item_size;

   if (ITEM_SIZE_FROM_MODEL(model, item_size)) return EINA_FALSE;

   item_size = efl_gfx_hint_size_min_get(entity);
   _size_to_model(model, item_size);
   return EINA_TRUE;
}

static Eina_Value
_entity_fetched_cb(Eo *obj, void *data, const Eina_Value v)
{
   MY_DATA_GET(obj, pd);
   Efl_Ui_Collection_Request *request = data;
   Efl_Gfx_Entity *child;
   unsigned int i, len;
   uint64_t updated_start_id;
   Eina_Bool updated = EINA_FALSE;

   EINA_VALUE_ARRAY_FOREACH(&v, len, i, child)
     {
        Efl_Ui_Collection_Item_Lookup *lookup;
        uint64_t search_index;
        unsigned int v;

        efl_key_data_set(child, COLLECTION_VIEW_MANAGED, COLLECTION_VIEW_MANAGED_YES);

        for (v = 0; v < 3; ++v)
          {
             if (!pd->viewport[v]) continue;

             if ((pd->viewport[v]->offset <= request->offset + i) &&
                 (request->offset + i < pd->viewport[v]->offset + pd->viewport[v]->count))
               {
                  uint64_t index = request->offset + i - pd->viewport[v]->offset;

                  efl_replace(&pd->viewport[v]->items[index].entity, child);
                  if (_entity_propagate(pd->viewport[v]->items[index].model, child))
                    {
                       if (!updated)
                         {
                            updated = EINA_TRUE;
                            updated_start_id = index;
                         }
                    }
                  else
                    {
                       if (updated)
                         {
                            efl_ui_position_manager_entity_item_size_changed(pd->manager,
                                                                             updated_start_id,
                                                                             index - 1);
                            updated = EINA_FALSE;
                         }
                    }
                  child = NULL;
                  break;
               }
          }

        // When requesting an entity, the model should already be in the cache
        if (!child) continue;

        search_index = request->offset + i;

        lookup = (void*) eina_rbtree_inline_lookup(pd->cache, &search_index,
                                                   sizeof (search_index), _cache_tree_lookup,
                                                   NULL);

        if (!lookup) continue;

        lookup->item.entity = efl_ref(child);

        if (_entity_propagate(lookup->item.model, child))
          {
             if (!updated)
               {
                  updated = EINA_TRUE;
                  updated_start_id = search_index;
               }
          }
        else
          {
             if (updated)
               {
                  efl_ui_position_manager_entity_item_size_changed(pd->manager,
                                                                   updated_start_id,
                                                                   search_index - 1);
                  updated = EINA_FALSE;
               }
          }
     }

   return v;
}

static void
_entity_free_cb(Eo *o, void *data, const Eina_Future *dead_future EINA_UNUSED)
{
   MY_DATA_GET(o, pd);
   Efl_Ui_Collection_Request *request = data;

   pd->requests = eina_list_remove(pd->requests, request);
   free(request);
}

static Eina_List *
_cache_size_fetch(Eina_List *requests, Efl_Ui_Collection_Request **request,
                  Efl_Ui_Collection_View_Data *pd,
                  uint64_t search_index,
                  Efl_Ui_Position_Manager_Batch_Size_Access *target,
                  Eina_Size2D item_base)
{
   Efl_Ui_Collection_Item_Lookup *lookup;
   Efl_Model *model;
   Eina_Size2D item_size;

   if (!pd->cache) goto not_found;

   lookup = (void*) eina_rbtree_inline_lookup(pd->cache, &search_index,
                                              sizeof (search_index), _cache_tree_lookup,
                                              NULL);
   if (!lookup) goto not_found;

   // In the cache we should always have model, so no need to check for it
   model = lookup->item.model;

   // If we do not know the size
   if (!ITEM_SIZE_FROM_MODEL(model, item_size))
     {
        // But can calculate it now
        if (!lookup->item.entity) goto not_found;

        item_size = efl_gfx_hint_size_min_get(lookup->item.entity);
        _size_to_model(model, item_size);
     }

   target->size = item_size;
   target->group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;

   return requests;

 not_found:
   requests = _request_add(requests, request, search_index, EINA_FALSE);

   target->size = item_base;
   target->group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;

   return requests;
}

static Eina_List *
_cache_entity_fetch(Eina_List *requests, Efl_Ui_Collection_Request **request,
                    Efl_Ui_Collection_View_Data *pd,
                    uint64_t search_index,
                    Efl_Ui_Position_Manager_Batch_Entity_Access *target)
{
   Efl_Ui_Collection_Item_Lookup *lookup;

   if (!pd->cache) goto not_found;

   lookup = (void*) eina_rbtree_inline_lookup(pd->cache, &search_index,
                                              sizeof (search_index), _cache_tree_lookup,
                                              NULL);
   if (!lookup) goto not_found;
   if (!lookup->item.entity) goto not_found;

   target->entity = lookup->item.entity;
   target->group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;

   return requests;

 not_found:
   requests = _request_add(requests, request, search_index, EINA_TRUE);

   target->entity = NULL;
   target->group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;

   return requests;
}

static void
_entity_request(Efl_Ui_Collection_View *obj, Efl_Ui_Collection_Request *request)
{
   request->f = efl_future_then(obj, request->f,
                                .success_type = EINA_VALUE_TYPE_ARRAY,
                                .success = _entity_fetch_cb);
   request->f = efl_future_then(obj, request->f,
                                .success_type = EINA_VALUE_TYPE_ARRAY,
                                .success = _entity_fetched_cb,
                                .data = request,
                                .free = _entity_free_cb);
}

static inline void
_entity_inflight_request(Efl_Ui_Collection_View *obj,
                         Efl_Ui_Collection_Request *request,
                         Efl_Ui_Collection_Request *inflight)
{
   if (request->entity_requested == EINA_FALSE) return ;
   if (request->entity_requested == inflight->entity_requested) return ;

   _entity_request(obj, inflight);
   inflight->entity_requested = EINA_TRUE;
}

static Eina_List *
_batch_request_flush(Eina_List *requests,
                     Efl_Ui_Collection_View *obj,
                     Efl_Ui_Collection_View_Data *pd)
{
   Efl_Ui_Collection_Request *request;

   EINA_LIST_FREE(requests, request)
     {
        // Check request intersection with all pending request
        Efl_Ui_Collection_Request *inflight;
        Efl_Model *model;
        Eina_List *l;

        EINA_LIST_FOREACH(pd->requests, l, inflight)
          {
             uint64_t istart = inflight->offset;
             uint64_t iend = inflight->offset + inflight->length;
             uint64_t rstart = request->offset;
             uint64_t rend = request->offset + request->length;

             // Way before
             if (rend < istart) continue;
             // Way after
             if (rstart >= iend) continue;

             // request included in current inflight request
             if (rstart >= istart && rend < iend)
               {
                  _entity_inflight_request(obj, request, inflight);

                  // In this case no need to start a request
                  free(request);
                  request = NULL;
                  break;
               }

             // request overflow left and right
             if (rstart < istart && iend < rend)
               {
                  // Remove the center portion of the request by emitting a new one
                  Efl_Ui_Collection_Request *rn;

                  rn = calloc(1, sizeof (Efl_Ui_Collection_Request));
                  if (!rn) break;

                  rn->offset = iend;
                  rn->length = rend - iend;
                  rn->model_requested = request->model_requested;
                  rn->entity_requested = request->entity_requested;

                  requests = eina_list_append(requests, rn);

                  request->length = istart - rstart;
                  _entity_inflight_request(obj, request, inflight);

                  continue;
               }

             // request overflow left
             if (rstart < istart && iend > istart && rend < iend)
               {
                  request->length = istart - rstart;
                  _entity_inflight_request(obj, request, inflight);
                  continue;
               }

             // request overflow right
             if (rstart >= istart && rstart < rend && iend < rend)
               {
                  request->offset = iend;
                  request->length = rend - iend;
                  _entity_inflight_request(obj, request, inflight);
                  continue;
               }
          }

        if (!request) continue;

        model = pd->model;
        // Are we ready yet
        if (!model)
          {
             free(request);
             continue;
          }
        // Is the request inside the limit of the model?
        if (request->offset >= efl_model_children_count_get(model))
          {
             free(request);
             continue;
          }
        // Is its limit outside the model limit?
        if (request->offset + request->length >= efl_model_children_count_get(model))
          {
             request->length = efl_model_children_count_get(model) - request->offset;
          }

        // We now have a request, time to trigger a fetch
        // We assume here that we are always fetching the model (model_requested must be true)
        if (!request->model_requested)
          {
             ERR("Someone forgot to set model_requested for %lu to %lu.",
                 request->offset, request->offset + request->length);
             request->model_requested = EINA_TRUE;
          }
        request->f = efl_model_children_slice_get(model, request->offset, request->length);
        request->f = efl_future_then(obj, request->f,
                                     .success = _model_fetched_cb,
                                     .data = request,
                                     .free = _model_free_cb);

        if (request->entity_requested)
          _entity_request(obj, request);

        pd->requests = eina_list_append(pd->requests, request);
     }

   return NULL;
}

static Efl_Ui_Position_Manager_Batch_Result
_batch_size_cb(void *data, int start_id, Eina_Rw_Slice memory)
{
   MY_DATA_GET(data, pd);
   Efl_Ui_Position_Manager_Batch_Size_Access *sizes;
   Efl_Ui_Collection_Request *request = NULL;
   Efl_Ui_Position_Manager_Batch_Result result = {-1, 0};
   Efl_Model *parent;
   Eina_List *requests = NULL;
   Eina_Size2D item_base;
   unsigned int i, count, limit;
   unsigned int idx = 0;

   // get the approximate value from the tree node
   parent = pd->model;
   if (!ITEM_BASE_SIZE_FROM_MODEL(parent, item_base))
     {
        item_base.w = 0;
        item_base.h = 0;
     }
   pd->last_base = item_base;

   sizes = memory.mem;
   count = efl_model_children_count_get(parent);
   limit = MIN(count - start_id, memory.len);

   // Look in the temporary cache now for the beginning of the buffer
   if (pd->viewport[0] && ((uint64_t)(start_id + idx) < pd->viewport[0]->offset))
     {
        while ((uint64_t)(start_id + idx) < pd->viewport[0]->offset)
          {
             uint64_t search_index = start_id + idx;

             requests = _cache_size_fetch(requests, &request, pd,
                                          search_index, &sizes[idx], item_base);

             idx++;
          }
     }

   // Then look in our buffer view if the needed information can be found there
   for (i = 0; i < 3; ++i)
     {
        if (!pd->viewport[i]) continue;

        while (idx < limit &&
               (pd->viewport[i]->offset <= start_id + idx) &&
               (start_id + idx < (pd->viewport[i]->offset + pd->viewport[i]->count)))
          {
             uint16_t offset = start_id + idx - pd->viewport[i]->offset;
             Efl_Model *model = pd->viewport[i]->items[offset].model;
             Efl_Gfx_Entity *entity = pd->viewport[i]->items[offset].entity;
             Eina_Bool entity_request = EINA_FALSE;

             if (!model)
               {
                  Eina_Size2D item_size;
                  Eina_Bool found = EINA_FALSE;

                  if (ITEM_SIZE_FROM_MODEL(model, item_size))
                    found = EINA_TRUE;
                  if (!found && entity)
                    {
                       item_size = efl_gfx_hint_size_min_get(entity);
                       _size_to_model(model, item_size);
                       found = EINA_TRUE;
                    }

                  if (found)
                    {
                       sizes[idx].size = item_size;
                       sizes[idx].group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;
                       goto done;
                    }

                  // We will need an entity to calculate this size
                  entity_request = EINA_TRUE;
               }

             // No data, add to the requests
             requests = _request_add(requests, &request, start_id + idx, entity_request);

             sizes[idx].size = item_base;
             sizes[idx].group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;

          done:
             idx++;
          }
     }

   // Look in the temporary cache now for the end of the buffer
   while (idx < limit)
     {
        uint64_t search_index = start_id + idx;

        requests = _cache_size_fetch(requests, &request, pd,
                                     search_index, &sizes[idx], item_base);

        idx++;
     }

   // Done, but flush request first
   if (request) requests = eina_list_append(requests, request);

   requests = _batch_request_flush(requests, data, pd);

   // Get the amount of filled item
   result.filled_items = limit;

   return result;
}

static Efl_Ui_Position_Manager_Batch_Result
_batch_entity_cb(void *data, int start_id, Eina_Rw_Slice memory)
{
   MY_DATA_GET(data, pd);
   Efl_Ui_Position_Manager_Batch_Entity_Access *entities;
   Efl_Ui_Collection_Request *request = NULL;
   Efl_Ui_Position_Manager_Batch_Result result = {-1, 0};
   Eina_List *requests = NULL;
   Efl_Model *parent;
   unsigned int i, count, limit;
   unsigned int idx = 0;

   parent = pd->model;

   entities = memory.mem;
   count = efl_model_children_count_get(parent);
   limit = MIN(count - start_id, memory.len);

   // Look in the temporary cache now for the beginning of the buffer
   if (pd->viewport[0] && ((uint64_t)(start_id + idx) < pd->viewport[0]->offset))
     {
        while ((uint64_t)(start_id + idx) < pd->viewport[0]->offset)
          {
             uint64_t search_index = start_id + idx;

             requests = _cache_entity_fetch(requests, &request, pd,
                                            search_index, &entities[idx]);

             idx++;
          }
     }

   // Then look in our buffer view if the needed information can be found there
   for (i = 0; i < 3; ++i)
     {
        if (!pd->viewport[i]) continue;

        while (idx < limit &&
               (pd->viewport[i]->offset <= start_id + idx) &&
               (start_id + idx < (pd->viewport[i]->offset + pd->viewport[i]->count)))
          {
             uint16_t offset = start_id + idx - pd->viewport[i]->offset;
             Efl_Gfx_Entity *entity = pd->viewport[i]->items[offset].entity;

             if (!entity)
               {
                  // No data, add to the requests
                  requests = _request_add(requests, &request, start_id + idx, EINA_TRUE);

                  entities[idx].entity = NULL;
                  entities[idx].group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;
               }
             else
               {
                  entities[idx].entity = entity;
                  entities[idx].group = EFL_UI_POSITION_MANAGER_BATCH_GROUP_STATE_NO_GROUP;
               }

             idx++;
          }
     }

   // Look in the temporary cache now for the end of the buffer
   while (idx < limit)
     {
        uint64_t search_index = start_id + idx;

        requests = _cache_entity_fetch(requests, &request, pd,
                                       search_index, &entities[idx]);

        idx++;
     }

   // Done, but flush request first
   if (request) requests = eina_list_append(requests, request);

   requests = _batch_request_flush(requests, data, pd);

   // Get the amount of filled item
   result.filled_items = limit;

   return result;
}

static void
_batch_free_cb(void *data)
{
   efl_unref(data);
}

static void
flush_min_size(Eo *obj, Efl_Ui_Collection_View_Data *pd)
{
   Eina_Size2D tmp = pd->content_min_size;

   if (!pd->match_content.w)
     tmp.w = -1;

   if (!pd->match_content.h)
     tmp.h = -1;

   efl_gfx_hint_size_min_set(obj, tmp);
}

static void
_manager_content_size_changed_cb(void *data, const Efl_Event *ev)
{
   Eina_Size2D *size = ev->info;
   MY_DATA_GET(data, pd);

   efl_gfx_entity_size_set(pd->sizer, *size);
}

static void
_manager_content_min_size_changed_cb(void *data, const Efl_Event *ev)
{
   Eina_Size2D *size = ev->info;
   MY_DATA_GET(data, pd);

   pd->content_min_size = *size;

   flush_min_size(data, pd);
}

static Eina_List *
_viewport_walk_fill(Eina_List *requests,
                    Efl_Ui_Collection_View *obj EINA_UNUSED,
                    Efl_Ui_Collection_View_Data *pd,
                    Efl_Ui_Collection_Viewport *viewport)
{
   Efl_Ui_Collection_Request *current = NULL;
   unsigned int j;

   for (j = 0; j < viewport->count; j++)
     {
        Efl_Ui_Collection_Item_Lookup *lookup;
        uint64_t index = viewport->offset + j;

        if (viewport->items[j].model) goto check_entity;

        lookup = (void*) eina_rbtree_inline_lookup(pd->cache, &index,
                                                   sizeof (index), _cache_tree_lookup,
                                                   NULL);

        if (lookup)
          {
             efl_replace(&viewport->items[j].model, lookup->item.model);
             efl_replace(&viewport->items[j].entity, lookup->item.entity);
             efl_replace(&lookup->item.entity, NULL); // Necessary to avoid premature release

             pd->cache = eina_rbtree_inline_remove(pd->cache, EINA_RBTREE_GET(lookup),
                                                   _cache_tree_cmp, NULL);
             _cache_item_free(EINA_RBTREE_GET(lookup), pd);
          }

     check_entity:
        if (viewport->items[j].entity) continue ;

        requests = _request_add(requests, &current, index, EINA_TRUE);
     }

   // We do break request per viewport, just in case we generate to big batch at once
   if (current) requests = eina_list_append(requests, current);

   return requests;
}

static void
_manager_content_visible_range_changed_cb(void *data, const Efl_Event *ev)
{
   Efl_Ui_Position_Manager_Range_Update *event = ev->info;
   MY_DATA_GET(data, pd);
   Eina_List *requests = NULL;
   unsigned int baseid;
   unsigned int delta, marginup, margindown;
   uint64_t upperlimit_offset, lowerlimit_offset;
   unsigned int i;

   pd->start_id = event->start_id;
   pd->end_id = event->end_id;

   delta = pd->end_id - pd->start_id;

   // First time setting up the viewport, so trigger request as we see fit
   if (!pd->viewport[0])
     {
        Eina_List *requests = NULL;

        baseid = (pd->start_id < delta) ? 0 : pd->start_id - delta;

        for (i = 0; i < 3; i++)
          {
             pd->viewport[i] = calloc(1, sizeof (Efl_Ui_Collection_Viewport));
             if (!pd->viewport[i]) continue;

             pd->viewport[i]->offset = baseid + delta * i;
             pd->viewport[i]->count = delta;
             pd->viewport[i]->items = calloc(delta, sizeof (Efl_Ui_Collection_Item));
             if (!pd->viewport[i]->items) continue ;

             requests = _viewport_walk_fill(requests, data, pd, pd->viewport[i]);
          }

        goto flush_requests;
     }

   // Compute limit offset
   upperlimit_offset = delta * 3 + pd->viewport[0]->offset;
   lowerlimit_offset = 0;

   // Adjust the viewport for size or to much offset change in two step

   // Trying to resize first if there size is in bigger/smaller than 25% of the original size
   margindown = delta * 75 / 100;
   marginup = delta * 125 / 100;
   if (margindown < pd->viewport[0]->count &&
       pd->viewport[0]->count < marginup)
     {
        // Trying to do the resize in an optimized way is complex, let's do it simple
        Efl_Ui_Collection_Item *items[3];
        unsigned int j = 0, t = 1;

        for (i = 0; i < 3; i++)
          {
             unsigned int m;

             items[i] = calloc(delta, sizeof (Efl_Ui_Collection_Item));
             if (!items[i]) continue;

             for (m = 0; m < delta && t < 3; m++)
               {
                  items[i][m] = pd->viewport[t]->items[j];

                  j++;
                  if (j < pd->viewport[t]->count) continue;

                  j = 0;
                  t++;
                  if (t == 3) break;
               }

             // Preserve last updated index to later build a request
             if (t == 3)
               {
                  upperlimit_offset = pd->viewport[0]->offset + i * delta + m;

                  t = 4; // So that we never come back here again
               }
          }

        // For now destroy leftover object, could be cached
        for (i = t; i < 3; i++)
          {
             for (; j < pd->viewport[i]->count; j++)
               {
                  _item_cleanup(pd->factory, &pd->viewport[i]->items[j]);
               }
             j = 0;
          }

        // And now define viewport back
        for (i = 0; i < 3; i++)
          {
             free(pd->viewport[i]->items);
             pd->viewport[i]->items = items[i];
             pd->viewport[i]->count = delta;
             pd->viewport[i]->offset = pd->viewport[0]->offset + delta * i;
          }
     }

   // We decided that resizing was unecessary
   delta = pd->viewport[0]->count;

   // Try to keep the visual viewport in between half of the first and last viewport

   // start_id is in the first half of the first viewport, assume upward move
   // start_id + delta is in the second half of the last viewport, assume upward move
   if (pd->viewport[0]->offset + delta / 2 < pd->start_id ||
       pd->start_id + delta > pd->viewport[2]->offset + delta / 2)
     {
        // We could optimize this to actually just move viewport around in most cases
        Efl_Ui_Collection_Item *items[3];
        unsigned int j = 0, t = 0;
        uint64_t target, current;

        // Case where are at the top
        if (pd->start_id < delta && pd->viewport[0]->offset == 0) goto build_request;

        // Trying to adjust the offset to maintain it in the center viewport +/- delta/2
        baseid = (pd->start_id < delta) ? 0 : pd->start_id - delta;

        // Lookup for starting point
        lowerlimit_offset = pd->viewport[0]->offset;
        target = baseid;

        // cleanup before target
        for (current = pd->viewport[0]->offset; current < target; current++)
          {
             _item_cleanup(pd->factory, &pd->viewport[t]->items[j]);

             j++;
             if (j < pd->viewport[t]->count) continue;

             j = 0;
             t++;
             if (t == 3) break;
          }

        // Allocation and copy
        for (i = 0; i < 3; i++)
          {
             unsigned int m;

             items[i] = calloc(delta, sizeof (Efl_Ui_Collection_Item));
             if (!items[i]) continue;

             for (m = 0; m < delta && t < 3; m++, target++)
               {
                  if (target < pd->viewport[t]->offset) continue ;
                  items[i][m] = pd->viewport[t]->items[j];

                  j++;
                  if (j < pd->viewport[t]->count) continue;

                  j = 0;
                  t++;
                  if (t == 3) break;
               }

             // Preserve last updated index to later build a request
             if (t == 3)
               {
                  if (upperlimit_offset > pd->viewport[0]->offset + i * delta + m)
                    {
                       upperlimit_offset = pd->viewport[0]->offset + i * delta + m;
                    }

                  t = 4; // So that we never come back here again
               }
          }

        // For now destroy leftover object, could be cached
        for (i = t; i < 3; i++)
          {
             for (; j < pd->viewport[i]->count; j++)
               {
                  _item_cleanup(pd->factory, &pd->viewport[i]->items[j]);
               }
             j = 0;
          }

        // And now define viewport back
        for (i = 0; i < 3; i++)
          {
             free(pd->viewport[i]->items);
             pd->viewport[i]->items = items[i];
             pd->viewport[i]->offset = baseid + delta * i;
          }
     }

 build_request:
   // Check if the first viewport has all the lower part of it filled with objects
   if (pd->viewport[0]->offset < lowerlimit_offset)
     {
        Efl_Ui_Collection_Request *request;

        request = calloc(1, sizeof (Efl_Ui_Collection_Request));
        if (request) return ;

        request->offset = lowerlimit_offset;
        // This length work over multiple viewport as they are contiguous
        request->length = lowerlimit_offset - pd->viewport[0]->offset;
        request->model_requested = EINA_TRUE;
        request->entity_requested = EINA_TRUE;

        requests = eina_list_append(requests, request);
     }

   // Check if the last viewport has all the upper part of it filler with objects
   if (pd->viewport[2]->offset + pd->viewport[2]->count > upperlimit_offset)
     {
        Efl_Ui_Collection_Request *request;

        request = calloc(1, sizeof (Efl_Ui_Collection_Request));
        if (request) return ;

        request->offset = upperlimit_offset;
        // This length work over multiple viewport as they are contiguous
        request->length = pd->viewport[2]->offset + pd->viewport[2]->count - upperlimit_offset;
        request->model_requested = EINA_TRUE;
        request->entity_requested = EINA_TRUE;

        requests = eina_list_append(requests, request);
     }

 flush_requests:
   requests = _batch_request_flush(requests, data, pd);
}

EFL_CALLBACKS_ARRAY_DEFINE(manager_cbs,
 { EFL_UI_POSITION_MANAGER_ENTITY_EVENT_CONTENT_SIZE_CHANGED, _manager_content_size_changed_cb },
 { EFL_UI_POSITION_MANAGER_ENTITY_EVENT_CONTENT_MIN_SIZE_CHANGED, _manager_content_min_size_changed_cb },
 { EFL_UI_POSITION_MANAGER_ENTITY_EVENT_VISIBLE_RANGE_CHANGED, _manager_content_visible_range_changed_cb }
)

static void
_item_scroll_internal(Eo *obj EINA_UNUSED,
                      Efl_Ui_Collection_View_Data *pd,
                      uint64_t index,
                      double align EINA_UNUSED,
                      Eina_Bool anim)
{
   Eina_Rect ipos, view;
   Eina_Position2D vpos;

   if (!pd->scroller) return;

   ipos = efl_ui_position_manager_entity_position_single_item(pd->manager, index);
   view = efl_ui_scrollable_viewport_geometry_get(pd->scroller);
   vpos = efl_ui_scrollable_content_pos_get(pd->scroller);

   ipos.x = ipos.x + vpos.x - view.x;
   ipos.y = ipos.y + vpos.y - view.y;

   //FIXME scrollable needs some sort of align, the docs do not even garantee to completly move in the element
   efl_ui_scrollable_scroll(pd->scroller, ipos, anim);
}

// Exported function

static void
_efl_ui_collection_view_factory_set(Eo *obj EINA_UNUSED, Efl_Ui_Collection_View_Data *pd,
                                  Efl_Ui_Factory *factory)
{
   efl_replace(&pd->factory, factory);
}

static Efl_Ui_Factory *
_efl_ui_collection_view_factory_get(const Eo *obj EINA_UNUSED, Efl_Ui_Collection_View_Data *pd)
{
   return pd->factory;
}

static void
_efl_ui_collection_view_position_manager_set(Eo *obj, Efl_Ui_Collection_View_Data *pd,
                                             Efl_Ui_Position_Manager_Entity *manager)
{
   Efl_Model *model;
   unsigned int count;

   if (manager)
     EINA_SAFETY_ON_FALSE_RETURN(efl_isa(manager, EFL_UI_POSITION_MANAGER_ENTITY_INTERFACE));

   if (pd->manager)
     {
        efl_event_callback_array_del(pd->manager, manager_cbs(), obj);
        efl_ui_position_manager_entity_data_access_set(pd->manager,
                                                       NULL, NULL, NULL,
                                                       NULL, NULL, NULL,
                                                       0);
        efl_del(pd->manager);
     }
   pd->manager = manager;
   if (!pd->manager) return;

   // Start watching change on model from here on
   model = pd->model;
   count = model ? efl_model_children_count_get(model) : 0;

   efl_parent_set(pd->manager, obj);
   efl_event_callback_array_add(pd->manager, manager_cbs(), obj);
   efl_ui_position_manager_entity_data_access_set(pd->manager,
                                                  efl_ref(obj), _batch_entity_cb, _batch_free_cb,
                                                  efl_ref(obj), _batch_size_cb, _batch_free_cb,
                                                  count);
   if (efl_finalized_get(obj))
     efl_ui_position_manager_entity_viewport_set(pd->manager, efl_ui_scrollable_viewport_geometry_get(obj));
   efl_ui_layout_orientation_set(pd->manager, pd->direction);
}

static Efl_Ui_Position_Manager_Entity *
_efl_ui_collection_view_position_manager_get(const Eo *obj EINA_UNUSED,
                                             Efl_Ui_Collection_View_Data *pd)
{
   return pd->manager;
}

static void
_efl_model_count_changed(void *data EINA_UNUSED, const Efl_Event *event EINA_UNUSED)
{
   // We are not triggering efl_ui_position_manager_entity_data_access_set as it is can
   // only be slow, we rely on child added/removed instead (If we were to not rely on
   // child added/removed we could maybe use count changed)
}

static void
_efl_model_properties_changed(void *data EINA_UNUSED, const Efl_Event *event EINA_UNUSED)
{
   // We could here watch if the global base size item change and notify of a global change
   // But I can not find a proper way to do it for the object that are not visible, which
   // is kind of the point...
}

static void
_efl_model_child_added(void *data, const Efl_Event *event)
{
   // At the moment model only append child, but let's try to handle it theorically correct
   Efl_Model_Children_Event *ev = event->info;
   MY_DATA_GET(data, pd);
   Eina_List *requests = NULL;
   unsigned int i;

   _cache_cleanup(pd);

   // Check if we really have something to do
   if (!pd->viewport[0]) goto notify_manager;

   // Insert the child in the viewport if necessary
   for (i = 0; i < 3; i++)
     {
        Efl_Ui_Collection_Request *request;
        unsigned int o;
        unsigned int j;

        if (ev->index < pd->viewport[i]->offset)
          {
             pd->viewport[i]->offset++;
             continue;
          }
        if (pd->viewport[i]->offset + pd->viewport[i]->count < ev->index)
          {
             continue;
          }

        for (j = 2; j > i; j--)
          {
             _item_cleanup(pd->factory, &pd->viewport[j]->items[pd->viewport[j]->count - 1]);
             memmove(&pd->viewport[j]->items[1],
                     &pd->viewport[j]->items[0],
                     (pd->viewport[j]->count - 1) * sizeof (Efl_Ui_Collection_Item));
             pd->viewport[j]->items[0] = pd->viewport[j - 1]->items[pd->viewport[j - 1]->count - 1];
             pd->viewport[j - 1]->items[pd->viewport[j - 1]->count - 1].entity = NULL;
             pd->viewport[j - 1]->items[pd->viewport[j - 1]->count - 1].model = NULL;
          }
        o = ev->index - pd->viewport[i]->offset;
        memmove(&pd->viewport[j]->items[o],
                &pd->viewport[j]->items[o + 1],
                (pd->viewport[j]->count - 1 - o) * sizeof (Efl_Ui_Collection_Item));
        pd->viewport[j]->items[o].entity = NULL;
        pd->viewport[j]->items[o].model = efl_ref(ev->child);

        request = calloc(1, sizeof (Efl_Ui_Collection_Request));
        if (!request) break;
        request->offset = ev->index;
        request->length = 1;
        request->model_requested = EINA_TRUE;
        request->entity_requested = EINA_TRUE;

        requests = eina_list_append(requests, request);

        requests = _batch_request_flush(requests, data, pd);

        break;
     }

 notify_manager:
   efl_ui_position_manager_entity_item_added(pd->manager, ev->index, NULL);
}

static void
_efl_model_child_removed(void *data, const Efl_Event *event)
{
   Efl_Model_Children_Event *ev = event->info;
   MY_DATA_GET(data, pd);
   Eina_List *requests = NULL;
   unsigned int i;

   _cache_cleanup(pd);

   // Check if we really have something to do
   if (!pd->viewport[0]) goto notify_manager;

   // Insert the child in the viewport if necessary
   for (i = 0; i < 3; i++)
     {
        Efl_Ui_Collection_Request *request;
        unsigned int o;

        if (ev->index < pd->viewport[i]->offset)
          {
             pd->viewport[i]->offset--;
             continue;
          }
        if (pd->viewport[i]->offset + pd->viewport[i]->count < ev->index)
          {
             continue;
          }

        o = ev->index - pd->viewport[i]->offset;
        _item_cleanup(pd->factory, &pd->viewport[i]->items[o]);
        for (; i < 3; i++)
          {
             memmove(&pd->viewport[i]->items[o],
                     &pd->viewport[i]->items[o + 1],
                     (pd->viewport[i]->count - 1 - o) * sizeof (Efl_Ui_Collection_Item));
             if (i + 1 < 3)
               {
                  pd->viewport[i]->items[pd->viewport[i]->count - 1] = pd->viewport[i + 1]->items[0];
               }
             else
               {
                  pd->viewport[i]->items[pd->viewport[i]->count - 1].entity = NULL;
                  pd->viewport[i]->items[pd->viewport[i]->count - 1].model = NULL;
               }
             o = 0;
          }

        request = calloc(1, sizeof (Efl_Ui_Collection_Request));
        if (!request) break;
        request->offset = pd->viewport[2]->offset + pd->viewport[i]->count - 1;
        request->length = 1;
        request->model_requested = EINA_TRUE;
        request->entity_requested = EINA_TRUE;

        requests = eina_list_append(requests, request);

        requests = _batch_request_flush(requests, data, pd);

        break;
     }

 notify_manager:
   efl_ui_position_manager_entity_item_removed(pd->manager, ev->index, NULL);
}

EFL_CALLBACKS_ARRAY_DEFINE(model_cbs,
                           { EFL_MODEL_EVENT_CHILDREN_COUNT_CHANGED, _efl_model_count_changed },
                           { EFL_MODEL_EVENT_PROPERTIES_CHANGED, _efl_model_properties_changed },
                           { EFL_MODEL_EVENT_CHILD_ADDED, _efl_model_child_added },
                           { EFL_MODEL_EVENT_CHILD_REMOVED, _efl_model_child_removed })

static void
_efl_ui_collection_view_model_changed(void *data, const Efl_Event *event)
{
   Efl_Model_Changed_Event *ev = event->info;
   Eina_List *requests = NULL;
   MY_DATA_GET(data, pd);
   Eina_Iterator *it;
   const char *property;
   Efl_Model *model = NULL;
   unsigned int i, count;
   Eina_Bool selection = EINA_FALSE, sizing = EINA_FALSE;

   if (ev->previous) efl_event_callback_array_del(ev->previous, model_cbs(), data);
   if (ev->current) efl_event_callback_array_add(ev->current, model_cbs(), data);

   // Cleanup all object, pending request and refetch everything
   _all_cleanup(pd);

   efl_del(pd->model);
   pd->model = NULL;

   if (!ev->current) return ;

   it = efl_model_properties_get(ev->current);
   EINA_ITERATOR_FOREACH(it, property)
     {
        // Check if the model provide selection
        if (eina_streq(property, "child.selected"))
          selection = EINA_TRUE;
        // Check if the model provide sizing logic
        else if (eina_streq(property, _efl_model_property_itemw) ||
                 eina_streq(property, _efl_model_property_itemh))
          sizing = EINA_TRUE;
     }
   eina_iterator_free(it);

   // Push selection model first
   if (!selection) model = efl_add(EFL_SELECT_MODEL_CLASS, data,
                                   efl_ui_view_model_set(efl_added, ev->current));
   if (!sizing) model = efl_add(EFL_UI_HOMOGENEOUS_MODEL_CLASS, data,
                                efl_ui_view_model_set(efl_added, model ? model : ev->current));
   if (!model) model = efl_add(EFL_VIEW_MODEL_CLASS, data,
                               efl_ui_view_model_set(efl_added, ev->current));

   count = efl_model_children_count_get(model);
   efl_ui_position_manager_entity_data_access_set(pd->manager,
                                                  efl_ref(data), _batch_entity_cb, _batch_free_cb,
                                                  efl_ref(data), _batch_size_cb, _batch_free_cb,
                                                  count);

   for (i = 0; i < 3; i++)
     {
        Efl_Ui_Collection_Request *request;

        if (!pd->viewport[i]) continue ;
        if (pd->viewport[i]->count == 0) continue ;

        request = calloc(1, sizeof (Efl_Ui_Collection_Request));
        if (!request) continue ;

        request->offset = pd->viewport[i]->offset;
        request->length = pd->viewport[i]->count;
        request->model_requested = EINA_TRUE;
        request->entity_requested = EINA_TRUE;

        requests = eina_list_append(requests, request);
     }

   requests = _batch_request_flush(requests, data, pd);

   pd->model = model;
}

static void
_pan_viewport_changed_cb(void *data, const Efl_Event *ev EINA_UNUSED)
{
   MY_DATA_GET(data, pd);
   Eina_Rect rect = efl_ui_scrollable_viewport_geometry_get(data);

   efl_ui_position_manager_entity_viewport_set(pd->manager, rect);
}

static void
_pan_position_changed_cb(void *data, const Efl_Event *ev EINA_UNUSED)
{
   MY_DATA_GET(data, pd);
   Eina_Position2D pos = efl_ui_pan_position_get(pd->pan);
   Eina_Position2D max = efl_ui_pan_position_max_get(pd->pan);
   Eina_Vector2 rpos = {0.0, 0.0};

   if (max.x > 0.0)
     rpos.x = (double)pos.x/(double)max.x;
   if (max.y > 0.0)
     rpos.y = (double)pos.y/(double)max.y;

   efl_ui_position_manager_entity_scroll_position_set(pd->manager, rpos.x, rpos.y);
}

EFL_CALLBACKS_ARRAY_DEFINE(pan_events_cb,
  {EFL_UI_PAN_EVENT_PAN_POSITION_CHANGED, _pan_position_changed_cb},
  {EFL_UI_PAN_EVENT_PAN_VIEWPORT_CHANGED, _pan_viewport_changed_cb},
)

static Efl_Object *
_efl_ui_collection_view_efl_object_constructor(Eo *obj, Efl_Ui_Collection_View_Data *pd)
{
   pd->direction = EFL_UI_LAYOUT_ORIENTATION_VERTICAL;
   obj = efl_constructor(efl_super(obj, EFL_UI_COLLECTION_VIEW_CLASS));

   if (!elm_widget_theme_klass_get(obj))
     elm_widget_theme_klass_set(obj, "item_container");

   efl_wref_add(efl_add(EFL_CANVAS_RECTANGLE_CLASS, evas_object_evas_get(obj)), &pd->sizer);
   efl_gfx_color_set(pd->sizer, 0, 0, 0, 0);

   efl_wref_add(efl_add(EFL_UI_PAN_CLASS, obj), &pd->pan);
   efl_content_set(pd->pan, pd->sizer);
   efl_event_callback_array_add(pd->pan, pan_events_cb(), obj);

   efl_wref_add(efl_add(EFL_UI_SCROLL_MANAGER_CLASS, obj), &pd->scroller);
   efl_composite_attach(obj, pd->scroller);
   efl_ui_mirrored_set(pd->scroller, efl_ui_mirrored_get(obj));
   efl_ui_scroll_manager_pan_set(pd->scroller, pd->pan);

   efl_ui_scroll_connector_bind(obj, pd->scroller);

   efl_event_callback_add(obj, EFL_UI_VIEW_EVENT_MODEL_CHANGED,
                          _efl_ui_collection_view_model_changed, obj);

   return obj;
}

static void
_efl_ui_collection_view_efl_object_invalidate(Eo *obj,
                                              Efl_Ui_Collection_View_Data *pd)
{
   efl_ui_collection_view_position_manager_set(obj, NULL);
   efl_event_callback_del(obj, EFL_UI_VIEW_EVENT_MODEL_CHANGED,
                          _efl_ui_collection_view_model_changed, obj);

   _all_cleanup(pd);

   efl_invalidate(efl_super(obj, EFL_UI_COLLECTION_VIEW_CLASS));
}

static void
_efl_ui_collection_view_efl_ui_layout_orientable_orientation_set(Eo *obj EINA_UNUSED,
                                                                 Efl_Ui_Collection_View_Data *pd,
                                                                 Efl_Ui_Layout_Orientation dir)
{
   if (pd->direction == dir) return;

   pd->direction = dir;
   if (pd->manager) efl_ui_layout_orientation_set(pd->manager, dir);
}

static Efl_Ui_Layout_Orientation
_efl_ui_collection_view_efl_ui_layout_orientable_orientation_get(const Eo *obj EINA_UNUSED,
                                                                 Efl_Ui_Collection_View_Data *pd)
{
   return pd->direction;
}

static Eina_Error
_efl_ui_collection_view_efl_ui_widget_theme_apply(Eo *obj, Efl_Ui_Collection_View_Data *pd)
{
   Eina_Error res;

   ELM_WIDGET_DATA_GET_OR_RETURN(obj, wd, EFL_UI_THEME_APPLY_ERROR_GENERIC);
   res = efl_ui_widget_theme_apply(efl_super(obj, MY_CLASS));
   if (res == EFL_UI_THEME_APPLY_ERROR_GENERIC) return res;
   efl_ui_mirrored_set(pd->scroller, efl_ui_mirrored_get(obj));
   efl_content_set(efl_part(wd->resize_obj, "efl.content"), pd->pan);

   return res;
}

static void
_efl_ui_collection_view_efl_ui_scrollable_interactive_match_content_set(Eo *obj, Efl_Ui_Collection_View_Data *pd, Eina_Bool w, Eina_Bool h)
{
   if (pd->match_content.w == w && pd->match_content.h == h)
     return;

   pd->match_content.w = w;
   pd->match_content.h = h;

   efl_ui_scrollable_match_content_set(pd->scroller, w, h);
   flush_min_size(obj, pd);
}

static void
_efl_ui_collection_view_efl_ui_multi_selectable_select_mode_set(Eo *obj EINA_UNUSED,
                                                                Efl_Ui_Collection_View_Data *pd,
                                                                Efl_Ui_Select_Mode mode)
{
   pd->mode = mode;
}

static Efl_Ui_Select_Mode
_efl_ui_collection_view_efl_ui_multi_selectable_select_mode_get(const Eo *obj EINA_UNUSED,
                                                                Efl_Ui_Collection_View_Data *pd)
{
   return pd->mode;
}

static Efl_Ui_Focus_Manager *
_efl_ui_collection_view_efl_ui_widget_focus_manager_focus_manager_create(Eo *obj, Efl_Ui_Collection_View_Data *pd EINA_UNUSED, Efl_Ui_Focus_Object *root)
{
   Efl_Ui_Collection_View_Focus_Manager_Data *mpd;
   Eo *manager = efl_add(EFL_UI_COLLECTION_VIEW_FOCUS_MANAGER_CLASS, obj,
                         efl_ui_focus_manager_root_set(efl_added, root));

   mpd = efl_data_scope_get(manager, EFL_UI_COLLECTION_VIEW_FOCUS_MANAGER_CLASS);
   mpd->collection = obj;

   return manager;
}

static Efl_Ui_Focus_Object *
_efl_ui_collection_view_efl_ui_focus_manager_move(Eo *obj, Efl_Ui_Collection_View_Data *pd, Efl_Ui_Focus_Direction direction)
{
   Eo *new_obj, *focus;
   Eina_Size2D step;

   new_obj = efl_ui_focus_manager_move(efl_super(obj, MY_CLASS), direction);
   focus = efl_ui_focus_manager_focus_get(obj);
   step = efl_gfx_hint_size_min_get(focus);
   if (!new_obj)
     {
        Eina_Rect pos = efl_gfx_entity_geometry_get(focus);
        Eina_Rect view = efl_ui_scrollable_viewport_geometry_get(pd->scroller);
        Eina_Position2D vpos = efl_ui_scrollable_content_pos_get(pd->scroller);

        pos.x = pos.x + vpos.x - view.x;
        pos.y = pos.y + vpos.y - view.y;
        Eina_Position2D max = efl_ui_pan_position_max_get(pd->pan);

        if (direction == EFL_UI_FOCUS_DIRECTION_RIGHT)
          {
             if (pos.x < max.x)
               {
                  pos.x = MIN(max.x, pos.x + step.w);
                  efl_ui_scrollable_scroll(obj, pos, EINA_TRUE);
                  new_obj = focus;
               }
          }
        else if (direction == EFL_UI_FOCUS_DIRECTION_LEFT)
          {
             if (pos.x > 0)
               {
                  pos.x = MAX(0, pos.x - step.w);
                  efl_ui_scrollable_scroll(obj, pos, EINA_TRUE);
                  new_obj = focus;
               }
          }
        else if (direction == EFL_UI_FOCUS_DIRECTION_UP)
          {
             if (pos.y > 0)
               {
                  pos.y = MAX(0, pos.y - step.h);
                  efl_ui_scrollable_scroll(obj, pos, EINA_TRUE);
                  new_obj = focus;
               }
          }
        else if (direction == EFL_UI_FOCUS_DIRECTION_DOWN)
          {
             if (pos.y < max.y)
               {
                  pos.y = MAX(0, pos.y + step.h);
                  efl_ui_scrollable_scroll(obj, pos, EINA_TRUE);
                  new_obj = focus;
               }
          }
     }
   else
     {
        Efl_Model *model;
        Eina_Value *vindex;
        uint64_t index;

        model = efl_ui_view_model_get(new_obj);
        vindex = efl_model_property_get(model, "child.index");
        if (eina_value_uint64_convert(vindex, &index))
          _item_scroll_internal(obj, pd, index, .0, EINA_TRUE);
        eina_value_free(vindex);
     }

   return new_obj;
}

#include "efl_ui_collection_view.eo.c"

#define ITEM_IS_OUTSIDE_VISIBLE(id) id < cpd->start_id || id > cpd->end_id

static Efl_Ui_Item *
_find_item(Eo *obj EINA_UNUSED, Efl_Ui_Collection_View_Data *pd EINA_UNUSED, Eo *focused_element)
{
   if (!focused_element) return NULL;

   while (focused_element &&
          efl_key_data_get(focused_element, COLLECTION_VIEW_MANAGED) != COLLECTION_VIEW_MANAGED_YES)
     {
        focused_element = efl_ui_widget_parent_get(focused_element);
     }

   return focused_element;
}

static inline void
_assert_item_available(Eo *item, int new_id, Efl_Ui_Collection_View_Data *pd)
{
   efl_gfx_entity_visible_set(item, EINA_TRUE);
   efl_gfx_entity_geometry_set(item, efl_ui_position_manager_entity_position_single_item(pd->manager, new_id));
}
static void
_efl_ui_collection_view_focus_manager_efl_ui_focus_manager_manager_focus_set(Eo *obj, Efl_Ui_Collection_View_Focus_Manager_Data *pd, Efl_Ui_Focus_Object *focus)
{
   MY_DATA_GET(pd->collection, cpd);
   Efl_Ui_Item *item = NULL;
   uint64_t item_id;

   if (focus == efl_ui_focus_manager_root_get(obj))
     {
        // Find last item
        item_id = efl_model_children_count_get(cpd->model) - 1;
     }
   else
     {
        Efl_Model *model;
        Eina_Value *vindex;

        item = _find_item(obj, cpd, focus);
        if (!item) return ;

        model = efl_ui_view_model_get(item);
        vindex = efl_model_property_get(model, "child.index");
        if (!eina_value_uint64_convert(vindex, &item_id)) return;
        eina_value_free(vindex);
     }

   // If this is NULL then we are before finalize, we cannot serve any sane value here
   if (!cpd->manager) return ;

   if (ITEM_IS_OUTSIDE_VISIBLE(item_id))
     {
        _assert_item_available(item, item_id, cpd);
     }
   efl_ui_focus_manager_focus_set(efl_super(obj, EFL_UI_COLLECTION_VIEW_FOCUS_MANAGER_CLASS), focus);
}

static Efl_Ui_Focus_Object *
_efl_ui_collection_view_focus_manager_efl_ui_focus_manager_request_move(Eo *obj, Efl_Ui_Collection_View_Focus_Manager_Data *pd, Efl_Ui_Focus_Direction direction, Efl_Ui_Focus_Object *child, Eina_Bool logical)
{
   MY_DATA_GET(pd->collection, cpd);
   Efl_Ui_Item *new_item, *item;
   unsigned int item_id;

   if (!child)
     child = efl_ui_focus_manager_focus_get(obj);

   item = _find_item(obj, cpd, child);

   //if this is NULL then we are before finalize, we cannot serve any sane value here
   if (!cpd->manager) return NULL;
   if (!item) return NULL;

   item_id = efl_ui_item_index_get(item);

   if (ITEM_IS_OUTSIDE_VISIBLE(item_id))
     {
        int new_id;

        new_id = efl_ui_position_manager_entity_relative_item(cpd->manager,
                                                              efl_ui_item_index_get(item),
                                                              direction);
        if (new_id == -1)
          {
             new_item = NULL;
          }
        else
          {
             unsigned int i;

             for (i = 0; i < 3; i++)
               {
                  if (!cpd->viewport[i]) continue;

                  if (!((cpd->viewport[i]->offset <= (unsigned int) new_id) &&
                        ((unsigned int) new_id < cpd->viewport[i]->offset + cpd->viewport[i]->count)))
                    continue;

                  new_item = cpd->viewport[i]->items[new_id - cpd->viewport[i]->offset].entity;
                  // We shouldn't get in a case where the available item is NULL
                  if (!new_item) break; // Just in case
                  _assert_item_available(new_item, new_id, cpd);
               }
          }
     }
   else
     {
        new_item = efl_ui_focus_manager_request_move(efl_super(obj, EFL_UI_COLLECTION_VIEW_FOCUS_MANAGER_CLASS), direction, child, logical);
     }

   return new_item;
}

#include "efl_ui_collection_view_focus_manager.eo.c"