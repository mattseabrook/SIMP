/* LIBGIMP - The GIMP Library
 * Copyright (C) 1995-2000 Peter Mattis and Spencer Kimball
 *
 * gimpproceduraldb.h
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

#if !defined (__GIMP_H_INSIDE__) && !defined (GIMP_COMPILATION)
#error "Only <libgimp/gimp.h> can be included directly."
#endif

#ifndef __GIMP_PROCEDURAL_DB_H__
#define __GIMP_PROCEDURAL_DB_H__

G_BEGIN_DECLS

/* For information look into the C source or the html documentation */


gboolean gimp_procedural_db_proc_info    (const gchar      *procedure,
                                          gchar           **blurb,
                                          gchar           **help,
                                          gchar           **author,
                                          gchar           **copyright,
                                          gchar           **date,
                                          GimpPDBProcType  *proc_type,
                                          gint             *num_args,
                                          gint             *num_values,
                                          GimpParamDef    **args,
                                          GimpParamDef    **return_vals);
gboolean gimp_procedural_db_get_data     (const gchar      *identifier,
                                          gpointer          data);
gboolean gimp_procedural_db_set_data     (const gchar      *identifier,
                                          gconstpointer     data,
                                          guint32           bytes);


G_END_DECLS

#endif /* __GIMP_PROCEDURAL_DB_H__ */
