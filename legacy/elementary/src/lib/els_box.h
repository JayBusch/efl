Evas_Object *_els_smart_box_add               (Evas *evas);
void         _els_smart_box_orientation_set   (Evas_Object *obj, int horizontal);
int          _els_smart_box_orientation_get   (Evas_Object *obj);
void         _els_smart_box_homogenous_set    (Evas_Object *obj, int homogenous);
int          _els_smart_box_pack_start        (Evas_Object *obj, Evas_Object *child);
int          _els_smart_box_pack_end          (Evas_Object *obj, Evas_Object *child);
int          _els_smart_box_pack_before       (Evas_Object *obj, Evas_Object *child, Evas_Object *before);
int          _els_smart_box_pack_after        (Evas_Object *obj, Evas_Object *child, Evas_Object *after);
void         _els_smart_box_unpack            (Evas_Object *obj);
