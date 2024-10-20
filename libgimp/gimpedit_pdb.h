/* LIBGIMP - The GIMP Library
 * Copyright (C) 1995-2003 Peter Mattis and Spencer Kimball
 *
 * gimpedit_pdb.h
 *
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

/* NOTE: This file is auto-generated by pdbgen.pl */

#if !defined (__GIMP_H_INSIDE__) && !defined (GIMP_COMPILATION)
#error "Only <libgimp/gimp.h> can be included directly."
#endif

#ifndef __GIMP_EDIT_PDB_H__
#define __GIMP_EDIT_PDB_H__

G_BEGIN_DECLS

/* For information look into the C source or the html documentation */


gboolean gimp_edit_cut                      (gint32               drawable_ID);
gboolean gimp_edit_copy                     (gint32               drawable_ID);
gboolean gimp_edit_copy_visible             (gint32               image_ID);
gint32   gimp_edit_paste                    (gint32               drawable_ID,
                                             gboolean             paste_into);
gint32   gimp_edit_paste_as_new_image       (void);
gchar*   gimp_edit_named_cut                (gint32               drawable_ID,
                                             const gchar         *buffer_name);
gchar*   gimp_edit_named_copy               (gint32               drawable_ID,
                                             const gchar         *buffer_name);
gchar*   gimp_edit_named_copy_visible       (gint32               image_ID,
                                             const gchar         *buffer_name);
gint32   gimp_edit_named_paste              (gint32               drawable_ID,
                                             const gchar         *buffer_name,
                                             gboolean             paste_into);
gint32   gimp_edit_named_paste_as_new_image (const gchar         *buffer_name);
GIMP_DEPRECATED_FOR(gimp_drawable_edit_clear)
gboolean gimp_edit_clear                    (gint32               drawable_ID);
GIMP_DEPRECATED_FOR(gimp_drawable_edit_fill)
gboolean gimp_edit_fill                     (gint32               drawable_ID,
                                             GimpFillType         fill_type);
GIMP_DEPRECATED_FOR(gimp_drawable_edit_bucket_fill)
gboolean gimp_edit_bucket_fill              (gint32               drawable_ID,
                                             GimpBucketFillMode   fill_mode,
                                             GimpLayerMode        paint_mode,
                                             gdouble              opacity,
                                             gdouble              threshold,
                                             gboolean             sample_merged,
                                             gdouble              x,
                                             gdouble              y);
GIMP_DEPRECATED_FOR(gimp_drawable_edit_bucket_fill)
gboolean gimp_edit_bucket_fill_full         (gint32               drawable_ID,
                                             GimpBucketFillMode   fill_mode,
                                             GimpLayerMode        paint_mode,
                                             gdouble              opacity,
                                             gdouble              threshold,
                                             gboolean             sample_merged,
                                             gboolean             fill_transparent,
                                             GimpSelectCriterion  select_criterion,
                                             gdouble              x,
                                             gdouble              y);
GIMP_DEPRECATED_FOR(gimp_drawable_edit_gradient_fill)
gboolean gimp_edit_blend                    (gint32               drawable_ID,
                                             GimpBlendMode        blend_mode,
                                             GimpLayerMode        paint_mode,
                                             GimpGradientType     gradient_type,
                                             gdouble              opacity,
                                             gdouble              offset,
                                             GimpRepeatMode       repeat,
                                             gboolean             reverse,
                                             gboolean             supersample,
                                             gint                 max_depth,
                                             gdouble              threshold,
                                             gboolean             dither,
                                             gdouble              x1,
                                             gdouble              y1,
                                             gdouble              x2,
                                             gdouble              y2);
GIMP_DEPRECATED_FOR(gimp_drawable_edit_stroke_selection)
gboolean gimp_edit_stroke                   (gint32               drawable_ID);
GIMP_DEPRECATED_FOR(gimp_drawable_edit_stroke_item)
gboolean gimp_edit_stroke_vectors           (gint32               drawable_ID,
                                             gint32               vectors_ID);


G_END_DECLS

#endif /* __GIMP_EDIT_PDB_H__ */
