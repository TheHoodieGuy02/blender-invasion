/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup edtransform
 *
 * \name 2D Transform Gizmo
 *
 * Used for UV/Image Editor
 */

#include "MEM_guardedalloc.h"

#include "BLI_math.h"

#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.h"
#include "BKE_editmesh.h"
#include "BKE_layer.h"

#include "RNA_access.h"

#include "UI_resources.h"
#include "UI_view2d.h"

#include "WM_api.h"
#include "WM_types.h"
#include "wm.h" /* XXX */

#include "ED_image.h"
#include "ED_screen.h"
#include "ED_uvedit.h"
#include "ED_gizmo_library.h"

#include "transform.h" /* own include */

/* -------------------------------------------------------------------- */
/** \name Arrow / Cage Gizmo Group
 *
 * Defines public functions, not the gizmo it's self:
 *
 * - #ED_widgetgroup_gizmo2d_xform_setup
 * - #ED_widgetgroup_gizmo2d_xform_refresh
 * - #ED_widgetgroup_gizmo2d_xform_draw_prepare
 * - #ED_widgetgroup_gizmo2d_xform_poll
 *
 * \{ */

/* axes as index */
enum {
  MAN2D_AXIS_TRANS_X = 0,
  MAN2D_AXIS_TRANS_Y,

  MAN2D_AXIS_LAST,
};

typedef struct GizmoGroup2D {
  wmGizmo *translate_xy[3];
  wmGizmo *cage;

  /* Current origin in view space, used to update widget origin for possible view changes */
  float origin[2];
  float min[2];
  float max[2];

  bool no_cage;

} GizmoGroup2D;

/* **************** Utilities **************** */

static void gizmo2d_get_axis_color(const int axis_idx, float *r_col, float *r_col_hi)
{
  const float alpha = 0.6f;
  const float alpha_hi = 1.0f;
  int col_id;

  switch (axis_idx) {
    case MAN2D_AXIS_TRANS_X:
      col_id = TH_AXIS_X;
      break;
    case MAN2D_AXIS_TRANS_Y:
      col_id = TH_AXIS_Y;
      break;
    default:
      BLI_assert(0);
      col_id = TH_AXIS_Y;
      break;
  }

  UI_GetThemeColor4fv(col_id, r_col);

  copy_v4_v4(r_col_hi, r_col);
  r_col[3] *= alpha;
  r_col_hi[3] *= alpha_hi;
}

static GizmoGroup2D *gizmogroup2d_init(wmGizmoGroup *gzgroup)
{
  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_2d", true);
  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_2d", true);
  const wmGizmoType *gzt_button = WM_gizmotype_find("GIZMO_GT_button_2d", true);

  GizmoGroup2D *ggd = MEM_callocN(sizeof(GizmoGroup2D), __func__);

  ggd->translate_xy[0] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, NULL);
  ggd->translate_xy[1] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, NULL);
  ggd->translate_xy[2] = WM_gizmo_new_ptr(gzt_button, gzgroup, NULL);
  ggd->cage = WM_gizmo_new_ptr(gzt_cage, gzgroup, NULL);

  RNA_enum_set(ggd->cage->ptr,
               "transform",
               ED_GIZMO_CAGE2D_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE2D_XFORM_FLAG_SCALE |
                   ED_GIZMO_CAGE2D_XFORM_FLAG_ROTATE);

  return ggd;
}

/**
 * Calculates origin in view space, use with #gizmo2d_origin_to_region.
 */
static void gizmo2d_calc_bounds(const bContext *C, float *r_center, float *r_min, float *r_max)
{
  float min_buf[2], max_buf[2];
  if (r_min == NULL) {
    r_min = min_buf;
  }
  if (r_max == NULL) {
    r_max = max_buf;
  }

  ScrArea *sa = CTX_wm_area(C);
  if (sa->spacetype == SPACE_IMAGE) {
    SpaceImage *sima = sa->spacedata.first;
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Image *ima = ED_space_image(sima);
    uint objects_len = 0;
    Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data_with_uvs(
        view_layer, NULL, &objects_len);
    if (!ED_uvedit_minmax_multi(CTX_data_scene(C), ima, objects, objects_len, r_min, r_max)) {
      zero_v2(r_min);
      zero_v2(r_max);
    }
    MEM_freeN(objects);
  }
  else {
    zero_v2(r_min);
    zero_v2(r_max);
  }
  mid_v2_v2v2(r_center, r_min, r_max);
}

/**
 * Convert origin (or any other point) from view to region space.
 */
BLI_INLINE void gizmo2d_origin_to_region(ARegion *ar, float *r_origin)
{
  UI_view2d_view_to_region_fl(&ar->v2d, r_origin[0], r_origin[1], &r_origin[0], &r_origin[1]);
}

/**
 * Custom handler for gizmo widgets
 */
static int gizmo2d_modal(bContext *C,
                         wmGizmo *widget,
                         const wmEvent *UNUSED(event),
                         eWM_GizmoFlagTweak UNUSED(tweak_flag))
{
  ARegion *ar = CTX_wm_region(C);
  float origin[3];

  gizmo2d_calc_bounds(C, origin, NULL, NULL);
  gizmo2d_origin_to_region(ar, origin);
  WM_gizmo_set_matrix_location(widget, origin);

  ED_region_tag_redraw(ar);

  return OPERATOR_RUNNING_MODAL;
}

void ED_widgetgroup_gizmo2d_xform_setup(const bContext *UNUSED(C), wmGizmoGroup *gzgroup)
{
  wmOperatorType *ot_translate = WM_operatortype_find("TRANSFORM_OT_translate", true);
  GizmoGroup2D *ggd = gizmogroup2d_init(gzgroup);
  gzgroup->customdata = ggd;

  for (int i = 0; i < ARRAY_SIZE(ggd->translate_xy); i++) {
    wmGizmo *gz = ggd->translate_xy[i];
    const float offset[3] = {0.0f, 0.2f};

    /* custom handler! */
    WM_gizmo_set_fn_custom_modal(gz, gizmo2d_modal);
    WM_gizmo_set_scale(gz, U.gizmo_size);

    if (i < 2) {
      float color[4], color_hi[4];
      gizmo2d_get_axis_color(i, color, color_hi);

      /* set up widget data */
      RNA_float_set(gz->ptr, "angle", -M_PI_2 * i);
      RNA_float_set(gz->ptr, "length", 0.8f);
      WM_gizmo_set_matrix_offset_location(gz, offset);
      WM_gizmo_set_line_width(gz, GIZMO_AXIS_LINE_WIDTH);
      WM_gizmo_set_color(gz, color);
      WM_gizmo_set_color_highlight(gz, color_hi);
    }
    else {
      PropertyRNA *prop = RNA_struct_find_property(gz->ptr, "icon");
      RNA_property_enum_set(gz->ptr, prop, ICON_NONE);

      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_BACKDROP);
      /* Make the center low alpha. */
      WM_gizmo_set_line_width(gz, 2.0f);
      RNA_float_set(gz->ptr, "backdrop_fill_alpha", 0.0);
    }

    /* Assign operator. */
    PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_translate, NULL);
    if (i < 2) {
      bool constraint[3] = {0};
      constraint[(i + 1) % 2] = 1;
      if (RNA_struct_find_property(ptr, "constraint_axis")) {
        RNA_boolean_set_array(ptr, "constraint_axis", constraint);
      }
    }

    RNA_boolean_set(ptr, "release_confirm", 1);
  }

  {
    wmOperatorType *ot_resize = WM_operatortype_find("TRANSFORM_OT_resize", true);
    wmOperatorType *ot_rotate = WM_operatortype_find("TRANSFORM_OT_rotate", true);
    PointerRNA *ptr;

    /* assign operator */
    ptr = WM_gizmo_operator_set(ggd->cage, 0, ot_translate, NULL);
    RNA_boolean_set(ptr, "release_confirm", 1);

    bool constraint_x[3] = {1, 0, 0};
    bool constraint_y[3] = {0, 1, 0};

    ptr = WM_gizmo_operator_set(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_X, ot_resize, NULL);
    PropertyRNA *prop_release_confirm = RNA_struct_find_property(ptr, "release_confirm");
    PropertyRNA *prop_constraint_axis = RNA_struct_find_property(ptr, "constraint_axis");
    RNA_property_boolean_set_array(ptr, prop_constraint_axis, constraint_x);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
    ptr = WM_gizmo_operator_set(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_X, ot_resize, NULL);
    RNA_property_boolean_set_array(ptr, prop_constraint_axis, constraint_x);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
    ptr = WM_gizmo_operator_set(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_Y, ot_resize, NULL);
    RNA_property_boolean_set_array(ptr, prop_constraint_axis, constraint_y);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
    ptr = WM_gizmo_operator_set(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_Y, ot_resize, NULL);
    RNA_property_boolean_set_array(ptr, prop_constraint_axis, constraint_y);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);

    ptr = WM_gizmo_operator_set(
        ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MIN_Y, ot_resize, NULL);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
    ptr = WM_gizmo_operator_set(
        ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MAX_Y, ot_resize, NULL);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
    ptr = WM_gizmo_operator_set(
        ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MIN_Y, ot_resize, NULL);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
    ptr = WM_gizmo_operator_set(
        ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MAX_Y, ot_resize, NULL);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
    ptr = WM_gizmo_operator_set(ggd->cage, ED_GIZMO_CAGE2D_PART_ROTATE, ot_rotate, NULL);
    RNA_property_boolean_set(ptr, prop_release_confirm, true);
  }
}

void ED_widgetgroup_gizmo2d_xform_setup_no_cage(const bContext *C, wmGizmoGroup *gzgroup)
{
  ED_widgetgroup_gizmo2d_xform_setup(C, gzgroup);
  GizmoGroup2D *ggd = gzgroup->customdata;
  ggd->no_cage = true;
}

void ED_widgetgroup_gizmo2d_xform_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  GizmoGroup2D *ggd = gzgroup->customdata;
  float origin[3];
  gizmo2d_calc_bounds(C, origin, ggd->min, ggd->max);
  copy_v2_v2(ggd->origin, origin);
  bool show_cage = !ggd->no_cage && !equals_v2v2(ggd->min, ggd->max);

  if (gzgroup->type->flag & WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP) {
    Scene *scene = CTX_data_scene(C);
    if (scene->toolsettings->workspace_tool_type == SCE_WORKSPACE_TOOL_FALLBACK) {
      gzgroup->use_fallback_keymap = true;
    }
    else {
      gzgroup->use_fallback_keymap = false;
    }
  }

  if (show_cage) {
    ggd->cage->flag &= ~WM_GIZMO_HIDDEN;
    for (int i = 0; i < ARRAY_SIZE(ggd->translate_xy); i++) {
      wmGizmo *gz = ggd->translate_xy[i];
      gz->flag |= WM_GIZMO_HIDDEN;
    }
  }
  else {
    ggd->cage->flag |= WM_GIZMO_HIDDEN;
    for (int i = 0; i < ARRAY_SIZE(ggd->translate_xy); i++) {
      wmGizmo *gz = ggd->translate_xy[i];
      gz->flag &= ~WM_GIZMO_HIDDEN;
    }
  }

  if (show_cage) {
    wmGizmoOpElem *gzop;
    float mid[2];
    const float *min = ggd->min;
    const float *max = ggd->max;
    mid_v2_v2v2(mid, min, max);

    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_X);
    PropertyRNA *prop_center_override = RNA_struct_find_property(&gzop->ptr, "center_override");
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){max[0], mid[1], 0.0f});
    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_X);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){min[0], mid[1], 0.0f});
    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_Y);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){mid[0], max[1], 0.0f});
    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_Y);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){mid[0], min[1], 0.0f});

    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MIN_Y);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){max[0], max[1], 0.0f});
    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MAX_Y);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){max[0], min[1], 0.0f});
    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MIN_Y);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){min[0], max[1], 0.0f});
    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MAX_Y);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){min[0], min[1], 0.0f});

    gzop = WM_gizmo_operator_get(ggd->cage, ED_GIZMO_CAGE2D_PART_ROTATE);
    RNA_property_float_set_array(
        &gzop->ptr, prop_center_override, (float[3]){mid[0], mid[1], 0.0f});
  }
}

void ED_widgetgroup_gizmo2d_xform_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *ar = CTX_wm_region(C);
  GizmoGroup2D *ggd = gzgroup->customdata;
  float origin[3] = {UNPACK2(ggd->origin), 0.0f};
  float origin_aa[3] = {UNPACK2(ggd->origin), 0.0f};

  gizmo2d_origin_to_region(ar, origin);

  for (int i = 0; i < ARRAY_SIZE(ggd->translate_xy); i++) {
    wmGizmo *gz = ggd->translate_xy[i];
    WM_gizmo_set_matrix_location(gz, origin);
  }

  UI_view2d_view_to_region_m4(&ar->v2d, ggd->cage->matrix_space);
  WM_gizmo_set_matrix_offset_location(ggd->cage, origin_aa);
  ggd->cage->matrix_offset[0][0] = (ggd->max[0] - ggd->min[0]);
  ggd->cage->matrix_offset[1][1] = (ggd->max[1] - ggd->min[1]);
}

/* TODO (Julian)
 * - Called on every redraw, better to do a more simple poll and check for selection in _refresh
 * - UV editing only, could be expanded for other things.
 */
bool ED_widgetgroup_gizmo2d_xform_poll(const bContext *C, wmGizmoGroupType *UNUSED(gzgt))
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  Object *obedit = CTX_data_edit_object(C);

  if (ED_space_image_show_uvedit(sima, obedit)) {
    Image *ima = ED_space_image(sima);
    Scene *scene = CTX_data_scene(C);
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMFace *efa;
    BMLoop *l;
    BMIter iter, liter;

    const int cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

    /* check if there's a selected poly */
    BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
      if (!uvedit_face_visible_test(scene, obedit, ima, efa)) {
        continue;
      }

      BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
        if (uvedit_uv_select_test(scene, l, cd_loop_uv_offset)) {
          return true;
        }
      }
    }
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scale Handles
 *
 * Defines public functions, not the gizmo it's self:
 *
 * - #ED_widgetgroup_gizmo2d_resize_setup
 * - #ED_widgetgroup_gizmo2d_resize_refresh
 * - #ED_widgetgroup_gizmo2d_resize_draw_prepare
 * - #ED_widgetgroup_gizmo2d_resize_poll
 *
 * \{ */

typedef struct GizmoGroup_Resize2D {
  wmGizmo *gizmo_xy[3];
  float origin[2];
} GizmoGroup_Resize2D;

static GizmoGroup_Resize2D *gizmogroup2d_resize_init(wmGizmoGroup *gzgroup)
{
  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_2d", true);
  const wmGizmoType *gzt_button = WM_gizmotype_find("GIZMO_GT_button_2d", true);

  GizmoGroup_Resize2D *ggd = MEM_callocN(sizeof(GizmoGroup_Resize2D), __func__);

  ggd->gizmo_xy[0] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, NULL);
  ggd->gizmo_xy[1] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, NULL);
  ggd->gizmo_xy[2] = WM_gizmo_new_ptr(gzt_button, gzgroup, NULL);

  return ggd;
}

void ED_widgetgroup_gizmo2d_resize_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  GizmoGroup_Resize2D *ggd = gzgroup->customdata;
  float origin[3];
  gizmo2d_calc_bounds(C, origin, NULL, NULL);
  copy_v2_v2(ggd->origin, origin);
}

void ED_widgetgroup_gizmo2d_resize_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *ar = CTX_wm_region(C);
  GizmoGroup_Resize2D *ggd = gzgroup->customdata;
  float origin[3] = {UNPACK2(ggd->origin), 0.0f};

  if (gzgroup->type->flag & WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP) {
    Scene *scene = CTX_data_scene(C);
    if (scene->toolsettings->workspace_tool_type == SCE_WORKSPACE_TOOL_FALLBACK) {
      gzgroup->use_fallback_keymap = true;
    }
    else {
      gzgroup->use_fallback_keymap = false;
    }
  }

  gizmo2d_origin_to_region(ar, origin);

  for (int i = 0; i < ARRAY_SIZE(ggd->gizmo_xy); i++) {
    wmGizmo *gz = ggd->gizmo_xy[i];
    WM_gizmo_set_matrix_location(gz, origin);
  }
}

bool ED_widgetgroup_gizmo2d_resize_poll(const bContext *C, wmGizmoGroupType *UNUSED(gzgt))
{
  return ED_widgetgroup_gizmo2d_xform_poll(C, NULL);
}

void ED_widgetgroup_gizmo2d_resize_setup(const bContext *UNUSED(C), wmGizmoGroup *gzgroup)
{

  wmOperatorType *ot_resize = WM_operatortype_find("TRANSFORM_OT_resize", true);
  GizmoGroup_Resize2D *ggd = gizmogroup2d_resize_init(gzgroup);
  gzgroup->customdata = ggd;

  for (int i = 0; i < ARRAY_SIZE(ggd->gizmo_xy); i++) {
    wmGizmo *gz = ggd->gizmo_xy[i];

    /* custom handler! */
    WM_gizmo_set_fn_custom_modal(gz, gizmo2d_modal);
    WM_gizmo_set_scale(gz, U.gizmo_size);

    if (i < 2) {
      const float offset[3] = {0.0f, 0.2f};
      float color[4], color_hi[4];
      gizmo2d_get_axis_color(i, color, color_hi);

      /* set up widget data */
      RNA_float_set(gz->ptr, "angle", -M_PI_2 * i);
      RNA_float_set(gz->ptr, "length", 0.8f);
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_BOX);

      WM_gizmo_set_matrix_offset_location(gz, offset);
      WM_gizmo_set_line_width(gz, GIZMO_AXIS_LINE_WIDTH);
      WM_gizmo_set_color(gz, color);
      WM_gizmo_set_color_highlight(gz, color_hi);
    }
    else {
      PropertyRNA *prop = RNA_struct_find_property(gz->ptr, "icon");
      RNA_property_enum_set(gz->ptr, prop, ICON_NONE);

      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_BACKDROP);
      /* Make the center low alpha. */
      WM_gizmo_set_line_width(gz, 2.0f);
      RNA_float_set(gz->ptr, "backdrop_fill_alpha", 0.0);
    }

    /* Assign operator. */
    PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_resize, NULL);
    if (i < 2) {
      bool constraint[3] = {0};
      constraint[(i + 1) % 2] = 1;
      if (RNA_struct_find_property(ptr, "constraint_axis")) {
        RNA_boolean_set_array(ptr, "constraint_axis", constraint);
      }
    }
    RNA_boolean_set(ptr, "release_confirm", true);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rotate Handles
 *
 * Defines public functions, not the gizmo it's self:
 *
 * - #ED_widgetgroup_gizmo2d_rotate_setup
 * - #ED_widgetgroup_gizmo2d_rotate_refresh
 * - #ED_widgetgroup_gizmo2d_rotate_draw_prepare
 * - #ED_widgetgroup_gizmo2d_rotate_poll
 *
 * \{ */

typedef struct GizmoGroup_Rotate2D {
  wmGizmo *gizmo;
  float origin[2];
} GizmoGroup_Rotate2D;

static GizmoGroup_Rotate2D *gizmogroup2d_rotate_init(wmGizmoGroup *gzgroup)
{
  const wmGizmoType *gzt_button = WM_gizmotype_find("GIZMO_GT_button_2d", true);

  GizmoGroup_Rotate2D *ggd = MEM_callocN(sizeof(GizmoGroup_Rotate2D), __func__);

  ggd->gizmo = WM_gizmo_new_ptr(gzt_button, gzgroup, NULL);

  return ggd;
}

void ED_widgetgroup_gizmo2d_rotate_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  GizmoGroup_Rotate2D *ggd = gzgroup->customdata;
  float origin[3];
  gizmo2d_calc_bounds(C, origin, NULL, NULL);
  copy_v2_v2(ggd->origin, origin);
}

void ED_widgetgroup_gizmo2d_rotate_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *ar = CTX_wm_region(C);
  GizmoGroup_Rotate2D *ggd = gzgroup->customdata;
  float origin[3] = {UNPACK2(ggd->origin), 0.0f};

  if (gzgroup->type->flag & WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP) {
    Scene *scene = CTX_data_scene(C);
    if (scene->toolsettings->workspace_tool_type == SCE_WORKSPACE_TOOL_FALLBACK) {
      gzgroup->use_fallback_keymap = true;
    }
    else {
      gzgroup->use_fallback_keymap = false;
    }
  }

  gizmo2d_origin_to_region(ar, origin);

  wmGizmo *gz = ggd->gizmo;
  WM_gizmo_set_matrix_location(gz, origin);
}

bool ED_widgetgroup_gizmo2d_rotate_poll(const bContext *C, wmGizmoGroupType *UNUSED(gzgt))
{
  return ED_widgetgroup_gizmo2d_xform_poll(C, NULL);
}

void ED_widgetgroup_gizmo2d_rotate_setup(const bContext *UNUSED(C), wmGizmoGroup *gzgroup)
{

  wmOperatorType *ot_resize = WM_operatortype_find("TRANSFORM_OT_rotate", true);
  GizmoGroup_Rotate2D *ggd = gizmogroup2d_rotate_init(gzgroup);
  gzgroup->customdata = ggd;

  /* Other setup functions iterate over axis. */
  {
    wmGizmo *gz = ggd->gizmo;

    /* custom handler! */
    WM_gizmo_set_fn_custom_modal(gz, gizmo2d_modal);
    WM_gizmo_set_scale(gz, U.gizmo_size);

    {
      PropertyRNA *prop = RNA_struct_find_property(gz->ptr, "icon");
      RNA_property_enum_set(gz->ptr, prop, ICON_NONE);

      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_BACKDROP);
      /* Make the center low alpha. */
      WM_gizmo_set_line_width(gz, 2.0f);
      RNA_float_set(gz->ptr, "backdrop_fill_alpha", 0.0);
    }

    /* Assign operator. */
    PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_resize, NULL);
    RNA_boolean_set(ptr, "release_confirm", true);
  }
}

/** \} */
