#include <ctype.h>
#include <assert.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "eo_lexer.h"
#include "eolian_priv.h"

static Eina_Bool
_validate(Eolian_Object *obj)
{
   obj->validated = EINA_TRUE;
   return EINA_TRUE;
}

static Eina_Bool
_validate_docstr(const Eolian_Unit *src, Eina_Stringshare *str, const Eolian_Object *info)
{
   if (!str || !str[0]) return EINA_TRUE;

   Eina_Bool ret = EINA_TRUE;
   Eina_List *pl = eolian_documentation_string_split(str);
   char *par;
   EINA_LIST_FREE(pl, par)
     {
        const char *doc = par;
        Eolian_Doc_Token tok;
        eolian_doc_token_init(&tok);
        while (ret && (doc = eolian_documentation_tokenize(doc, &tok)))
          if (eolian_doc_token_type_get(&tok) == EOLIAN_DOC_TOKEN_REF)
            if (eolian_doc_token_ref_get(src, &tok, NULL, NULL) == EOLIAN_DOC_REF_INVALID)
              {
                 char *refn = eolian_doc_token_text_get(&tok);
                 _eolian_log_line(info->file, info->line, info->column,
                                  "failed validating reference '%s'", refn);
                 free(refn);
                 ret = EINA_FALSE;
                 break;
              }
        free(par);
     }

   return ret;
}

static Eina_Bool
_validate_doc(const Eolian_Unit *src, Eolian_Documentation *doc)
{
   if (!doc)
     return EINA_TRUE;

   if (!_validate_docstr(src, doc->summary, &doc->base))
     return EINA_FALSE;
   if (!_validate_docstr(src, doc->description, &doc->base))
     return EINA_FALSE;

   return _validate(&doc->base);
}

static Eina_Bool _validate_type(const Eolian_Unit *src, Eolian_Type *tp);
static Eina_Bool _validate_expr(const Eolian_Unit *src,
                                Eolian_Expression *expr,
                                const Eolian_Type *tp,
                                Eolian_Expression_Mask msk);
static Eina_Bool _validate_function(const Eolian_Unit *src,
                                    Eolian_Function *func,
                                    Eina_Hash *nhash);

typedef struct _Cb_Ret
{
   const Eolian_Unit *unit;
   Eina_Bool succ;
} Cb_Ret;

static Eina_Bool
_sf_map_cb(const Eina_Hash *hash EINA_UNUSED, const void *key EINA_UNUSED,
           const Eolian_Struct_Type_Field *sf, Cb_Ret *sc)
{
   sc->succ = _validate_type(sc->unit, sf->type);

   if (!sc->succ)
     return EINA_FALSE;

   sc->succ = _validate_doc(sc->unit, sf->doc);

   return sc->succ;
}

static Eina_Bool
_ef_map_cb(const Eina_Hash *hash EINA_UNUSED, const void *key EINA_UNUSED,
           const Eolian_Enum_Type_Field *ef, Cb_Ret *sc)
{
   if (ef->value)
     sc->succ = _validate_expr(sc->unit, ef->value, NULL, EOLIAN_MASK_INT);
   else
     sc->succ = EINA_TRUE;

   if (!sc->succ)
     return EINA_FALSE;

   sc->succ = _validate_doc(sc->unit, ef->doc);

   return sc->succ;
}

static Eina_Bool
_obj_error(const Eolian_Object *o, const char *msg)
{
   _eolian_log_line(o->file, o->line, o->column, "%s", msg);
   return EINA_FALSE;
}

static Eina_Bool
_validate_typedecl(const Eolian_Unit *src, Eolian_Typedecl *tp)
{
   if (tp->base.validated)
     return EINA_TRUE;

   if (!_validate_doc(src, tp->doc))
     return EINA_FALSE;

   switch (tp->type)
     {
      case EOLIAN_TYPEDECL_ALIAS:
        if (!_validate_type(src, tp->base_type))
          return EINA_FALSE;
        if (!tp->freefunc && tp->base_type->freefunc)
          tp->freefunc = eina_stringshare_ref(tp->base_type->freefunc);
        return _validate(&tp->base);
      case EOLIAN_TYPEDECL_STRUCT:
        {
           Cb_Ret rt = { src, EINA_TRUE };
           eina_hash_foreach(tp->fields, (Eina_Hash_Foreach)_sf_map_cb, &rt);
           if (!rt.succ)
             return EINA_FALSE;
           return _validate(&tp->base);
        }
      case EOLIAN_TYPEDECL_STRUCT_OPAQUE:
        return _validate(&tp->base);
      case EOLIAN_TYPEDECL_ENUM:
        {
           Cb_Ret rt = { src, EINA_TRUE };
           eina_hash_foreach(tp->fields, (Eina_Hash_Foreach)_ef_map_cb, &rt);
           if (!rt.succ)
             return EINA_FALSE;
           return _validate(&tp->base);
        }
      case EOLIAN_TYPEDECL_FUNCTION_POINTER:
        if (!_validate_function(src, tp->function_pointer, NULL))
          return EINA_FALSE;
        return _validate(&tp->base);
      default:
        return EINA_FALSE;
     }
   return _validate(&tp->base);
}

static const char * const eo_complex_frees[] =
{
   "eina_accessor_free", "eina_array_free",
   "eina_iterator_free", "eina_hash_free",
   "eina_list_free", "eina_inarray_free", "eina_inlist_free",

   "efl_del" /* future */
};

static const char *eo_obj_free = "efl_del";
static const char *eo_str_free = "free";
static const char *eo_strshare_free = "eina_stringshare_del";
static const char *eo_value_free = "eina_value_flush";
static const char *eo_value_ptr_free = "eina_value_free";

static Eina_Bool
_validate_type(const Eolian_Unit *src, Eolian_Type *tp)
{
   char buf[256];
   if (tp->owned && !database_type_is_ownable(src, tp))
     {
        snprintf(buf, sizeof(buf), "type '%s' is not ownable", tp->full_name);
        return _obj_error(&tp->base, buf);
     }

   if (tp->is_ptr && !tp->legacy)
     {
        tp->is_ptr = EINA_FALSE;
        Eina_Bool still_ownable = database_type_is_ownable(src, tp);
        tp->is_ptr = EINA_TRUE;
        if (still_ownable)
          {
             return _obj_error(&tp->base, "cannot take a pointer to pointer type");
          }
     }

   switch (tp->type)
     {
      case EOLIAN_TYPE_VOID:
      case EOLIAN_TYPE_UNDEFINED:
        return _validate(&tp->base);
      case EOLIAN_TYPE_REGULAR:
        {
           if (tp->base_type)
             {
                int kwid = eo_lexer_keyword_str_to_id(tp->full_name);
                if (!tp->freefunc)
                  {
                     tp->freefunc = eina_stringshare_add(eo_complex_frees[
                       kwid - KW_accessor]);
                  }
                Eolian_Type *itp = tp->base_type;
                /* validate types in brackets so freefuncs get written... */
                while (itp)
                  {
                     if (!_validate_type(src, itp))
                       return EINA_FALSE;
                     if ((kwid >= KW_accessor) && (kwid <= KW_list))
                       {
                          if (!database_type_is_ownable(src, itp))
                            {
                               snprintf(buf, sizeof(buf),
                                        "%s cannot contain value types (%s)",
                                        tp->full_name, itp->full_name);
                               return _obj_error(&itp->base, buf);
                            }
                       }
                     itp = itp->next_type;
                  }
                return _validate(&tp->base);
             }
           /* builtins */
           int id = eo_lexer_keyword_str_to_id(tp->full_name);
           if (id)
             {
                if (!eo_lexer_is_type_keyword(id))
                  return EINA_FALSE;
                if (!tp->freefunc)
                  switch (id)
                    {
                     case KW_string:
                       tp->freefunc = eina_stringshare_add(eo_str_free);
                       break;
                     case KW_stringshare:
                       tp->freefunc = eina_stringshare_add(eo_strshare_free);
                       break;
                     case KW_any_value:
                       tp->freefunc = eina_stringshare_add(eo_value_free);
                       break;
                     case KW_any_value_ptr:
                       tp->freefunc = eina_stringshare_add(eo_value_ptr_free);
                       break;
                     default:
                       break;
                    }
                return _validate(&tp->base);
             }
           /* user defined */
           tp->tdecl = database_type_decl_find(src, tp);
           if (!tp->tdecl)
             {
                snprintf(buf, sizeof(buf), "undefined type %s", tp->full_name);
                return _obj_error(&tp->base, buf);
             }
           if (!_validate_typedecl(src, tp->tdecl))
             return EINA_FALSE;
           if (tp->tdecl->freefunc && !tp->freefunc)
             tp->freefunc = eina_stringshare_ref(tp->tdecl->freefunc);
           return _validate(&tp->base);
        }
      case EOLIAN_TYPE_CLASS:
        {
           tp->klass = (Eolian_Class *)eolian_class_get_by_name(src, tp->full_name);
           if (!tp->klass)
             {
                snprintf(buf, sizeof(buf), "undefined class %s "
                         "(likely wrong namespacing)", tp->full_name);
                return _obj_error(&tp->base, buf);
             }
           if (!tp->freefunc)
             tp->freefunc = eina_stringshare_add(eo_obj_free);
           return _validate(&tp->base);
        }
      default:
        return EINA_FALSE;
     }
   return _validate(&tp->base);
}

static Eina_Bool
_validate_expr(const Eolian_Unit *src, Eolian_Expression *expr,
               const Eolian_Type *tp, Eolian_Expression_Mask msk)
{
   Eolian_Value val;
   if (tp)
     val = database_expr_eval_type(src, expr, tp);
   else
     val = database_expr_eval(src, expr, msk);

   if (val.type == EOLIAN_EXPR_UNKNOWN)
     return EINA_FALSE;

   return _validate(&expr->base);
}

static Eina_Bool
_validate_param(const Eolian_Unit *src, Eolian_Function_Parameter *param)
{
   if (!_validate_type(src, param->type))
     return EINA_FALSE;

   if (param->value && !_validate_expr(src, param->value, param->type, 0))
     return EINA_FALSE;

   if (!_validate_doc(src, param->doc))
     return EINA_FALSE;

   return _validate(&param->base);
}

static Eina_Bool
_validate_function(const Eolian_Unit *src, Eolian_Function *func, Eina_Hash *nhash)
{
   Eina_List *l;
   Eolian_Function_Parameter *param;
   char buf[512];

   static int _duplicates_warn = -1;
   if (EINA_UNLIKELY(_duplicates_warn < 0))
     {
        const char *s = getenv("EOLIAN_WARN_FUNC_DUPLICATES");
        if (!s) _duplicates_warn = 0;
        else _duplicates_warn = atoi(s);
     }

   const Eolian_Function *ofunc = nhash ? eina_hash_find(nhash, func->name) : NULL;
   if (EINA_UNLIKELY(ofunc && (ofunc != func) && (_duplicates_warn > 0)))
     {
        snprintf(buf, sizeof(buf),
                 "%sfunction '%s' redefined (originally at %s:%d:%d)",
                 func->is_beta ? "beta " : "", func->name, ofunc->base.file,
                 ofunc->base.line, ofunc->base.column);
        if ((!func->is_beta && !ofunc->is_beta) || (_duplicates_warn > 1))
          _obj_error(&func->base, buf);
        if (_duplicates_warn > 1)
          return EINA_FALSE;
     }

   /* if already validated, no need to perform the other checks...
    * but duplicate checks need to be performed every time
    */
   if (func->base.validated)
     {
        /* it might be validated, but need to add it anyway */
        if (!ofunc && nhash)
          eina_hash_add(nhash, func->name, func);
        return EINA_TRUE;
     }

   if (func->get_ret_type && !_validate_type(src, func->get_ret_type))
     return EINA_FALSE;

   if (func->set_ret_type && !_validate_type(src, func->set_ret_type))
     return EINA_FALSE;

   if (func->get_ret_val && !_validate_expr(src, func->get_ret_val,
                                            func->get_ret_type, 0))
     return EINA_FALSE;

   if (func->set_ret_val && !_validate_expr(src, func->set_ret_val,
                                            func->set_ret_type, 0))
     return EINA_FALSE;

#define EOLIAN_PARAMS_VALIDATE(params) \
   EINA_LIST_FOREACH(params, l, param) \
     if (!_validate_param(src, param)) \
       return EINA_FALSE;

   EOLIAN_PARAMS_VALIDATE(func->prop_values);
   EOLIAN_PARAMS_VALIDATE(func->prop_values_get);
   EOLIAN_PARAMS_VALIDATE(func->prop_values_set);
   EOLIAN_PARAMS_VALIDATE(func->prop_keys);
   EOLIAN_PARAMS_VALIDATE(func->prop_keys_get);
   EOLIAN_PARAMS_VALIDATE(func->prop_keys_set);

#undef EOLIAN_PARAMS_VALIDATE

   if (!_validate_doc(src, func->get_return_doc))
     return EINA_FALSE;
   if (!_validate_doc(src, func->set_return_doc))
     return EINA_FALSE;

   /* just for now, when dups become errors there will be no need to check */
   if (!ofunc && nhash)
     eina_hash_add(nhash, func->name, func);

   return _validate(&func->base);
}

static Eina_Bool
_validate_part(const Eolian_Unit *src, Eolian_Part *part, Eina_Hash *nhash)
{
   const Eolian_Function *ofunc = eina_hash_find(nhash, part->name);
   if (ofunc)
     {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "part '%s' conflicts with a function (defined at %s:%d:%d)",
                 part->name, ofunc->base.file,
                 ofunc->base.line, ofunc->base.column);
        _obj_error(&part->base, buf);
     }

   /* see _validate_function above */
   if (part->base.validated)
     return EINA_TRUE;

   if (!_validate_doc(src, part->doc))
     return EINA_FALSE;

   /* switch the class name for class */
   Eolian_Class *pcl = eina_hash_find(src->state->unit.classes, part->klass_name);
   if (!pcl)
     {
        char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), "unknown part class '%s' (incorrect case?)",
                 part->klass_name);
        _obj_error(&part->base, buf);
        return EINA_FALSE;
     }
   eina_stringshare_del(part->klass_name);
   part->klass = pcl;

   return _validate(&part->base);
}

static Eina_Bool
_validate_event(const Eolian_Unit *src, Eolian_Event *event)
{
   if (event->base.validated)
     return EINA_TRUE;

   if (event->type && !_validate_type(src, event->type))
     return EINA_FALSE;

   if (!_validate_doc(src, event->doc))
     return EINA_FALSE;

   return _validate(&event->base);
}

const Eolian_Class *
_get_impl_class(const Eolian_Class *cl, const char *cln)
{
   if (!cl || !strcmp(cl->full_name, cln))
     return cl;
   Eina_List *l;
   Eolian_Class *icl;
   EINA_LIST_FOREACH(cl->inherits, l, icl)
     {
        /* we can do a depth first search, it's easier and doesn't matter
         * which part of the inheritance tree we find the class in
         */
        const Eolian_Class *fcl = _get_impl_class(icl, cln);
        if (fcl)
          return fcl;
     }
   return NULL;
}

#define _eo_parser_log(_base, ...) \
   _eolian_log_line((_base)->file, (_base)->line, (_base)->column, __VA_ARGS__)

static Eina_Bool
_db_fill_implement(Eolian_Class *cl, Eolian_Implement *impl)
{
   Eolian_Function_Type ftype = EOLIAN_METHOD;

   if (impl->is_prop_get && impl->is_prop_set)
     ftype = EOLIAN_PROPERTY;
   else if (impl->is_prop_get)
     ftype = EOLIAN_PROP_GET;
   else if (impl->is_prop_set)
     ftype = EOLIAN_PROP_SET;

   size_t imlen = strlen(impl->full_name);
   char *clbuf = alloca(imlen + 1);
   memcpy(clbuf, impl->full_name, imlen + 1);

   char *ldot = strrchr(clbuf, '.');
   if (!ldot)
     return EINA_FALSE; /* unreachable in practice, for static analysis */

   *ldot = '\0'; /* split between class name and func name */
   const char *clname = clbuf;
   const char *fnname = ldot + 1;

   const Eolian_Class *tcl = _get_impl_class(cl, clname);
   if (!tcl)
     {
        _eo_parser_log(&impl->base, "class '%s' not found within the inheritance tree of '%s'",
                       clname, cl->full_name);
        return EINA_FALSE;
     }

   impl->klass = tcl;

   const Eolian_Function *fid = eolian_class_function_get_by_name(tcl, fnname, EOLIAN_UNRESOLVED);
   if (!fid)
     {
        _eo_parser_log(&impl->base, "function '%s' not known in class '%s'", fnname, clname);
        return EINA_FALSE;
     }

   Eolian_Function_Type aftype = eolian_function_type_get(fid);

   Eina_Bool auto_empty = (impl->get_auto || impl->get_empty);

   /* match implement type against function type */
   if (ftype == EOLIAN_PROPERTY)
     {
        /* property */
        if (aftype != EOLIAN_PROPERTY)
          {
             _eo_parser_log(&impl->base, "function '%s' is not a complete property", fnname);
             return EINA_FALSE;
          }
        auto_empty = auto_empty && (impl->set_auto || impl->set_empty);
     }
   else if (ftype == EOLIAN_PROP_SET)
     {
        /* setter */
        if ((aftype != EOLIAN_PROP_SET) && (aftype != EOLIAN_PROPERTY))
          {
             _eo_parser_log(&impl->base, "function '%s' doesn't have a setter", fnname);
             return EINA_FALSE;
          }
        auto_empty = (impl->set_auto || impl->set_empty);
     }
   else if (ftype == EOLIAN_PROP_GET)
     {
        /* getter */
        if ((aftype != EOLIAN_PROP_GET) && (aftype != EOLIAN_PROPERTY))
          {
             _eo_parser_log(&impl->base, "function '%s' doesn't have a getter", fnname);
             return EINA_FALSE;
          }
     }
   else if (aftype != EOLIAN_METHOD)
     {
        _eo_parser_log(&impl->base, "function '%s' is not a method", fnname);
        return EINA_FALSE;
     }

   if ((fid->klass == cl) && !auto_empty)
     {
        /* only allow explicit implements from other classes, besides auto and
         * empty... also prevents pure virtuals from being implemented
         */
        _eo_parser_log(&impl->base, "invalid implement '%s'", impl->full_name);
        return EINA_FALSE;
     }

   impl->foo_id = fid;

   return EINA_TRUE;
}

static Eina_Bool
_db_fill_implements(Eolian_Class *cl)
{
   Eolian_Implement *impl;
   Eina_List *l;

   Eina_Bool ret = EINA_TRUE;

   Eina_Hash *th = eina_hash_string_small_new(NULL),
             *pth = eina_hash_string_small_new(NULL);
   EINA_LIST_FOREACH(cl->implements, l, impl)
     {
        Eina_Bool prop = (impl->is_prop_get || impl->is_prop_set);
        if (eina_hash_find(prop ? pth : th, impl->full_name))
          {
             _eo_parser_log(&impl->base, "duplicate implement '%s'", impl->full_name);
             ret = EINA_FALSE;
             goto end;
          }
        if (impl->klass != cl)
          {
             if (!_db_fill_implement(cl, impl))
               {
                  ret = EINA_FALSE;
                  goto end;
               }
             if (eolian_function_is_constructor(impl->foo_id, impl->klass))
               database_function_constructor_add((Eolian_Function *)impl->foo_id, cl);
          }
        if ((impl->klass != cl) && !_db_fill_implement(cl, impl))
          {
             ret = EINA_FALSE;
             goto end;
          }
        eina_hash_add(prop ? pth : th, impl->full_name, impl->full_name);
     }

end:
   eina_hash_free(th);
   eina_hash_free(pth);
   return ret;
}

static Eina_Bool
_db_fill_ctors(Eolian_Class *cl)
{
   Eolian_Constructor *ctor;
   Eina_List *l;

   Eina_Bool ret = EINA_TRUE;

   Eina_Hash *th = eina_hash_string_small_new(NULL);
   EINA_LIST_FOREACH(cl->constructors, l, ctor)
     {
        if (eina_hash_find(th, ctor->full_name))
          {
             _eo_parser_log(&ctor->base, "duplicate ctor '%s'", ctor->full_name);
             ret = EINA_FALSE;
             goto end;
          }
        const char *ldot = strrchr(ctor->full_name, '.');
        if (!ldot)
          {
             ret = EINA_FALSE;
             goto end;
          }
        char *cnbuf = alloca(ldot - ctor->full_name + 1);
        memcpy(cnbuf, ctor->full_name, ldot - ctor->full_name);
        cnbuf[ldot - ctor->full_name] = '\0';
        const Eolian_Class *tcl = _get_impl_class(cl, cnbuf);
        if (!tcl)
          {
             _eo_parser_log(&ctor->base, "class '%s' not found within the inheritance tree of '%s'",
                            cnbuf, cl->full_name);
             ret = EINA_FALSE;
             goto end;
          }
        ctor->klass = tcl;
        const Eolian_Function *cfunc = eolian_constructor_function_get(ctor);
        if (!cfunc)
          {
             _eo_parser_log(&ctor->base, "unable to find function '%s'", ctor->full_name);
             ret = EINA_FALSE;
             goto end;
          }
        database_function_constructor_add((Eolian_Function *)cfunc, tcl);
        eina_hash_add(th, ctor->full_name, ctor->full_name);
     }

end:
   eina_hash_free(th);
   return ret;
}

static Eina_Bool
_db_fill_inherits(const Eolian_Unit *src, Eolian_Class *cl, Eina_Hash *fhash)
{
   if (eina_hash_find(fhash, cl->full_name))
     return EINA_TRUE;

   Eina_List *il = cl->inherits;
   Eina_Stringshare *inn = NULL;
   cl->inherits = NULL;
   Eina_Bool succ = EINA_TRUE;

   EINA_LIST_FREE(il, inn)
     {
        if (!succ)
          {
             eina_stringshare_del(inn);
             continue;
          }
        Eolian_Class *icl = eina_hash_find(src->state->unit.classes, inn);
        if (!icl)
          {
             succ = EINA_FALSE;
             char buf[PATH_MAX];
             snprintf(buf, sizeof(buf), "unknown inherit '%s' (incorrect case?)", inn);
             _obj_error(&cl->base, buf);
          }
        else
          {
             cl->inherits = eina_list_append(cl->inherits, icl);
             /* recursively fill so the tree is valid */
             if (!icl->base.validated && !_db_fill_inherits(src, icl, fhash))
               succ = EINA_FALSE;
          }
        eina_stringshare_del(inn);
     }

   eina_hash_add(fhash, cl->full_name, cl);

   /* make sure impls/ctors are filled first, but do it only once */
   if (!_db_fill_implements(cl))
     return EINA_FALSE;

   if (!_db_fill_ctors(cl))
     return EINA_FALSE;

   return succ;
}

static Eina_Bool
_validate_implement(const Eolian_Unit *src, Eolian_Implement *impl)
{
   if (impl->base.validated)
     return EINA_TRUE;

   if (!_validate_doc(src, impl->common_doc))
     return EINA_FALSE;
   if (!_validate_doc(src, impl->get_doc))
     return EINA_FALSE;
   if (!_validate_doc(src, impl->set_doc))
     return EINA_FALSE;

   return _validate(&impl->base);
}

static Eina_Bool
_validate_class(const Eolian_Unit *src, Eolian_Class *cl,
                Eina_Hash *nhash, Eina_Bool ipass)
{
   Eina_List *l;
   Eolian_Function *func;
   Eolian_Event *event;
   Eolian_Part *part;
   Eolian_Implement *impl;
   Eolian_Class *icl;

   if (!cl)
     return EINA_FALSE; /* if this happens something is very wrong though */

   Eina_Bool valid = cl->base.validated;

   /* refill inherits in the current inheritance tree first */
   if (!valid && !ipass)
     {
        Eina_Hash *fhash = eina_hash_stringshared_new(NULL);
        if (!_db_fill_inherits(src, cl, fhash))
          {
             eina_hash_free(fhash);
             return EINA_FALSE;
          }
        eina_hash_free(fhash);
     }

   EINA_LIST_FOREACH(cl->inherits, l, icl)
     {
        /* first inherit needs some checking done on it */
        if (!valid && (l == cl->inherits)) switch (cl->type)
          {
           case EOLIAN_CLASS_REGULAR:
           case EOLIAN_CLASS_ABSTRACT:
             if (icl->type != EOLIAN_CLASS_REGULAR && icl->type != EOLIAN_CLASS_ABSTRACT)
               {
                  char buf[PATH_MAX];
                  snprintf(buf, sizeof(buf), "regular classes ('%s') cannot inherit from non-regular classes ('%s')",
                           cl->full_name, icl->full_name);
                  return _obj_error(&cl->base, buf);
               }
             break;
           case EOLIAN_CLASS_MIXIN:
           case EOLIAN_CLASS_INTERFACE:
             if (icl->type != EOLIAN_CLASS_MIXIN && icl->type != EOLIAN_CLASS_INTERFACE)
               {
                  char buf[PATH_MAX];
                  snprintf(buf, sizeof(buf), "non-regular classes ('%s') cannot inherit from regular classes ('%s')",
                           cl->full_name, icl->full_name);
                  return _obj_error(&cl->base, buf);
               }
             break;
           default:
             break;
          }
        if (!_validate_class(src, icl, nhash, EINA_TRUE))
          return EINA_FALSE;
     }

   EINA_LIST_FOREACH(cl->properties, l, func)
     if (!_validate_function(src, func, nhash))
       return EINA_FALSE;

   EINA_LIST_FOREACH(cl->methods, l, func)
     if (!_validate_function(src, func, nhash))
       return EINA_FALSE;

   EINA_LIST_FOREACH(cl->events, l, event)
     if (!_validate_event(src, event))
       return EINA_FALSE;

   EINA_LIST_FOREACH(cl->parts, l, part)
     if (!_validate_part(src, part, nhash))
       return EINA_FALSE;

   EINA_LIST_FOREACH(cl->implements, l, impl)
     if (!_validate_implement(src, impl))
       return EINA_FALSE;

   /* all the checks that need to be done every time are performed now */
   if (valid)
     return EINA_TRUE;

   if (!_validate_doc(src, cl->doc))
     return EINA_FALSE;

   return _validate(&cl->base);
}

static Eina_Bool
_validate_variable(const Eolian_Unit *src, Eolian_Variable *var)
{
   if (var->base.validated)
     return EINA_TRUE;

   if (!_validate_type(src, var->base_type))
     return EINA_FALSE;

   if (var->value && !_validate_expr(src, var->value, var->base_type, 0))
     return EINA_FALSE;

   if (!_validate_doc(src, var->doc))
     return EINA_FALSE;

   return _validate(&var->base);
}

static Eina_Bool
_typedecl_map_cb(const Eina_Hash *hash EINA_UNUSED, const void *key EINA_UNUSED,
                 Eolian_Typedecl *tp, Cb_Ret *sc)
{
   return (sc->succ = _validate_typedecl(sc->unit, tp));
}

static Eina_Bool
_var_map_cb(const Eina_Hash *hash EINA_UNUSED, const void *key EINA_UNUSED,
             Eolian_Variable *var, Cb_Ret *sc)
{
   return (sc->succ = _validate_variable(sc->unit, var));
}

Eina_Bool
database_validate(Eolian *state, const Eolian_Unit *src)
{
   Eolian_Class *cl;

   Eina_Iterator *iter = eolian_all_classes_get(src);
   Eina_Hash *nhash = eina_hash_string_small_new(NULL);
   EINA_ITERATOR_FOREACH(iter, cl)
     {
        eina_hash_free_buckets(nhash);
        if (!_validate_class(src, cl, nhash, EINA_FALSE))
          {
             eina_iterator_free(iter);
             eina_hash_free(nhash);
             return EINA_FALSE;
          }
     }
   eina_hash_free(nhash);
   eina_iterator_free(iter);

   Cb_Ret rt = { src, EINA_TRUE };

   eina_hash_foreach(state->unit.aliases, (Eina_Hash_Foreach)_typedecl_map_cb, &rt);
   if (!rt.succ)
     return EINA_FALSE;

   eina_hash_foreach(state->unit.structs, (Eina_Hash_Foreach)_typedecl_map_cb, &rt);
   if (!rt.succ)
     return EINA_FALSE;

   eina_hash_foreach(state->unit.enums, (Eina_Hash_Foreach)_typedecl_map_cb, &rt);
   if (!rt.succ)
     return EINA_FALSE;

   eina_hash_foreach(state->unit.globals, (Eina_Hash_Foreach)_var_map_cb, &rt);
   if (!rt.succ)
     return EINA_FALSE;

   eina_hash_foreach(state->unit.constants, (Eina_Hash_Foreach)_var_map_cb, &rt);
   if (!rt.succ)
     return EINA_FALSE;

   return EINA_TRUE;
}
