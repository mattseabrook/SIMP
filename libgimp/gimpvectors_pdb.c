/* LIBGIMP - The GIMP Library
 * Copyright (C) 1995-2003 Peter Mattis and Spencer Kimball
 *
 * gimpvectors_pdb.c
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

#include "config.h"

#include <string.h>

#include "gimp.h"


/**
 * SECTION: gimpvectors
 * @title: gimpvectors
 * @short_description: Functions for querying and manipulating vectors.
 *
 * Functions for querying and manipulating vectors.
 **/


/**
 * gimp_vectors_new:
 * @image_ID: The image.
 * @name: the name of the new vector object.
 *
 * Creates a new empty vectors object.
 *
 * Creates a new empty vectors object. The vectors object needs to be
 * added to the image using gimp_image_insert_vectors().
 *
 * Returns: the current vector object, 0 if no vector exists in the
 * image.
 *
 * Since: 2.4
 **/
gint32
gimp_vectors_new (gint32       image_ID,
                  const gchar *name)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gint32 vectors_ID = -1;

  return_vals = gimp_run_procedure ("gimp-vectors-new",
                                    &nreturn_vals,
                                    GIMP_PDB_IMAGE, image_ID,
                                    GIMP_PDB_STRING, name,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    vectors_ID = return_vals[1].data.d_vectors;

  gimp_destroy_params (return_vals, nreturn_vals);

  return vectors_ID;
}

/**
 * gimp_vectors_new_from_text_layer:
 * @image_ID: The image.
 * @layer_ID: The text layer.
 *
 * Creates a new vectors object from a text layer.
 *
 * Creates a new vectors object from a text layer. The vectors object
 * needs to be added to the image using gimp_image_insert_vectors().
 *
 * Returns: The vectors of the text layer.
 *
 * Since: 2.6
 **/
gint32
gimp_vectors_new_from_text_layer (gint32 image_ID,
                                  gint32 layer_ID)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gint32 vectors_ID = -1;

  return_vals = gimp_run_procedure ("gimp-vectors-new-from-text-layer",
                                    &nreturn_vals,
                                    GIMP_PDB_IMAGE, image_ID,
                                    GIMP_PDB_LAYER, layer_ID,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    vectors_ID = return_vals[1].data.d_vectors;

  gimp_destroy_params (return_vals, nreturn_vals);

  return vectors_ID;
}

/**
 * gimp_vectors_copy:
 * @vectors_ID: The vectors object to copy.
 *
 * Copy a vectors object.
 *
 * This procedure copies the specified vectors object and returns the
 * copy.
 *
 * Returns: The newly copied vectors object.
 *
 * Since: 2.6
 **/
gint32
gimp_vectors_copy (gint32 vectors_ID)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gint32 vectors_copy_ID = -1;

  return_vals = gimp_run_procedure ("gimp-vectors-copy",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    vectors_copy_ID = return_vals[1].data.d_vectors;

  gimp_destroy_params (return_vals, nreturn_vals);

  return vectors_copy_ID;
}

/**
 * gimp_vectors_get_strokes:
 * @vectors_ID: The vectors object.
 * @num_strokes: The number of strokes returned.
 *
 * List the strokes associated with the passed path.
 *
 * Returns an Array with the stroke-IDs associated with the passed
 * path.
 *
 * Returns: List of the strokes belonging to the path.
 *
 * Since: 2.4
 **/
gint *
gimp_vectors_get_strokes (gint32  vectors_ID,
                          gint   *num_strokes)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gint *stroke_ids = NULL;

  return_vals = gimp_run_procedure ("gimp-vectors-get-strokes",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_END);

  *num_strokes = 0;

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    {
      *num_strokes = return_vals[1].data.d_int32;
      stroke_ids = g_new (gint32, *num_strokes);
      memcpy (stroke_ids,
              return_vals[2].data.d_int32array,
              *num_strokes * sizeof (gint32));
    }

  gimp_destroy_params (return_vals, nreturn_vals);

  return stroke_ids;
}

/**
 * gimp_vectors_stroke_get_length:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @precision: The precision used for the approximation.
 *
 * Measure the length of the given stroke.
 *
 * Measure the length of the given stroke.
 *
 * Returns: The length (in pixels) of the given stroke.
 *
 * Since: 2.4
 **/
gdouble
gimp_vectors_stroke_get_length (gint32  vectors_ID,
                                gint    stroke_id,
                                gdouble precision)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gdouble length = 0.0;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-get-length",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, precision,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    length = return_vals[1].data.d_float;

  gimp_destroy_params (return_vals, nreturn_vals);

  return length;
}

/**
 * gimp_vectors_stroke_get_point_at_dist:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @dist: The given distance.
 * @precision: The precision used for the approximation.
 * @x_point: The x position of the point.
 * @y_point: The y position of the point.
 * @slope: The slope (dy / dx) at the specified point.
 * @valid: Indicator for the validity of the returned data.
 *
 * Get point at a specified distance along the stroke.
 *
 * This will return the x,y position of a point at a given distance
 * along the stroke. The distance will be obtained by first digitizing
 * the curve internally and then walking along the curve. For a closed
 * stroke the start of the path is the first point on the path that was
 * created. This might not be obvious. If the stroke is not long
 * enough, a \"valid\" flag will be FALSE.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_stroke_get_point_at_dist (gint32    vectors_ID,
                                       gint      stroke_id,
                                       gdouble   dist,
                                       gdouble   precision,
                                       gdouble  *x_point,
                                       gdouble  *y_point,
                                       gdouble  *slope,
                                       gboolean *valid)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-get-point-at-dist",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, dist,
                                    GIMP_PDB_FLOAT, precision,
                                    GIMP_PDB_END);

  *x_point = 0.0;
  *y_point = 0.0;
  *slope = 0.0;
  *valid = FALSE;

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  if (success)
    {
      *x_point = return_vals[1].data.d_float;
      *y_point = return_vals[2].data.d_float;
      *slope = return_vals[3].data.d_float;
      *valid = return_vals[4].data.d_int32;
    }

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_remove_stroke:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 *
 * remove the stroke from a vectors object.
 *
 * Remove the stroke from a vectors object.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_remove_stroke (gint32 vectors_ID,
                            gint   stroke_id)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-remove-stroke",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_stroke_close:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 *
 * closes the specified stroke.
 *
 * Closes the specified stroke.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_stroke_close (gint32 vectors_ID,
                           gint   stroke_id)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-close",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_stroke_translate:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @off_x: Offset in x direction.
 * @off_y: Offset in y direction.
 *
 * translate the given stroke.
 *
 * Translate the given stroke.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_stroke_translate (gint32 vectors_ID,
                               gint   stroke_id,
                               gint   off_x,
                               gint   off_y)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-translate",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_INT32, off_x,
                                    GIMP_PDB_INT32, off_y,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_stroke_scale:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @scale_x: Scale factor in x direction.
 * @scale_y: Scale factor in y direction.
 *
 * scales the given stroke.
 *
 * Scale the given stroke.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_stroke_scale (gint32  vectors_ID,
                           gint    stroke_id,
                           gdouble scale_x,
                           gdouble scale_y)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-scale",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, scale_x,
                                    GIMP_PDB_FLOAT, scale_y,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_stroke_rotate:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @center_x: X coordinate of the rotation center.
 * @center_y: Y coordinate of the rotation center.
 * @angle: angle to rotate about.
 *
 * rotates the given stroke.
 *
 * Rotates the given stroke around given center by angle (in degrees).
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_stroke_rotate (gint32  vectors_ID,
                            gint    stroke_id,
                            gdouble center_x,
                            gdouble center_y,
                            gdouble angle)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-rotate",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, center_x,
                                    GIMP_PDB_FLOAT, center_y,
                                    GIMP_PDB_FLOAT, angle,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_stroke_flip:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @flip_type: Flip orientation, either vertical or horizontal.
 * @axis: axis coordinate about which to flip, in pixels.
 *
 * flips the given stroke.
 *
 * Rotates the given stroke around given center by angle (in degrees).
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_stroke_flip (gint32              vectors_ID,
                          gint                stroke_id,
                          GimpOrientationType flip_type,
                          gdouble             axis)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-flip",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_INT32, flip_type,
                                    GIMP_PDB_FLOAT, axis,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_stroke_flip_free:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @x1: X coordinate of the first point of the flipping axis.
 * @y1: Y coordinate of the first point of the flipping axis.
 * @x2: X coordinate of the second point of the flipping axis.
 * @y2: Y coordinate of the second point of the flipping axis.
 *
 * flips the given stroke about an arbitrary axis.
 *
 * Flips the given stroke about an arbitrary axis. Axis is defined by
 * two coordinates in the image (in pixels), through which the flipping
 * axis passes.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_stroke_flip_free (gint32  vectors_ID,
                               gint    stroke_id,
                               gdouble x1,
                               gdouble y1,
                               gdouble x2,
                               gdouble y2)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-flip-free",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, x1,
                                    GIMP_PDB_FLOAT, y1,
                                    GIMP_PDB_FLOAT, x2,
                                    GIMP_PDB_FLOAT, y2,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_stroke_get_points:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @num_points: The number of floats returned.
 * @controlpoints: List of the control points for the stroke (x0, y0, x1, y1, ...).
 * @closed: Whether the stroke is closed or not.
 *
 * returns the control points of a stroke.
 *
 * returns the control points of a stroke. The interpretation of the
 * coordinates returned depends on the type of the stroke. For Gimp 2.4
 * this is always a bezier stroke, where the coordinates are the
 * control points.
 *
 * Returns: type of the stroke (always GIMP_VECTORS_STROKE_TYPE_BEZIER
 * for now).
 *
 * Since: 2.4
 **/
GimpVectorsStrokeType
gimp_vectors_stroke_get_points (gint32     vectors_ID,
                                gint       stroke_id,
                                gint      *num_points,
                                gdouble  **controlpoints,
                                gboolean  *closed)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  GimpVectorsStrokeType type = 0;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-get-points",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_END);

  *num_points = 0;

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    {
      type = return_vals[1].data.d_int32;
      *num_points = return_vals[2].data.d_int32;
      *controlpoints = g_new (gdouble, *num_points);
      memcpy (*controlpoints,
              return_vals[3].data.d_floatarray,
              *num_points * sizeof (gdouble));
      *closed = return_vals[4].data.d_int32;
    }

  gimp_destroy_params (return_vals, nreturn_vals);

  return type;
}

/**
 * gimp_vectors_stroke_new_from_points:
 * @vectors_ID: The vectors object.
 * @type: type of the stroke (always GIMP_VECTORS_STROKE_TYPE_BEZIER for now).
 * @num_points: The number of elements in the array, i.e. the number of controlpoints in the stroke * 2 (x- and y-coordinate).
 * @controlpoints: List of the x- and y-coordinates of the control points.
 * @closed: Whether the stroke is to be closed or not.
 *
 * Adds a stroke of a given type to the vectors object.
 *
 * Adds a stroke of a given type to the vectors object. The coordinates
 * of the control points can be specified. For now only strokes of the
 * type GIMP_VECTORS_STROKE_TYPE_BEZIER are supported. The control
 * points are specified as a pair of float values for the x- and
 * y-coordinate. The Bezier stroke type needs a multiple of three
 * control points. Each Bezier segment endpoint (anchor, A) has two
 * additional control points (C) associated. They are specified in the
 * order CACCACCAC...
 *
 * Returns: The stroke ID of the newly created stroke.
 *
 * Since: 2.4
 **/
gint
gimp_vectors_stroke_new_from_points (gint32                 vectors_ID,
                                     GimpVectorsStrokeType  type,
                                     gint                   num_points,
                                     const gdouble         *controlpoints,
                                     gboolean               closed)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gint stroke_id = 0;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-new-from-points",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, type,
                                    GIMP_PDB_INT32, num_points,
                                    GIMP_PDB_FLOATARRAY, controlpoints,
                                    GIMP_PDB_INT32, closed,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    stroke_id = return_vals[1].data.d_int32;

  gimp_destroy_params (return_vals, nreturn_vals);

  return stroke_id;
}

/**
 * gimp_vectors_stroke_interpolate:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @precision: The precision used for the approximation.
 * @num_coords: The number of floats returned.
 * @closed: Whether the stroke is closed or not.
 *
 * returns polygonal approximation of the stroke.
 *
 * returns polygonal approximation of the stroke.
 *
 * Returns: List of the coords along the path (x0, y0, x1, y1, ...).
 *
 * Since: 2.4
 **/
gdouble *
gimp_vectors_stroke_interpolate (gint32    vectors_ID,
                                 gint      stroke_id,
                                 gdouble   precision,
                                 gint     *num_coords,
                                 gboolean *closed)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gdouble *coords = NULL;

  return_vals = gimp_run_procedure ("gimp-vectors-stroke-interpolate",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, precision,
                                    GIMP_PDB_END);

  *num_coords = 0;

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    {
      *num_coords = return_vals[1].data.d_int32;
      coords = g_new (gdouble, *num_coords);
      memcpy (coords,
              return_vals[2].data.d_floatarray,
              *num_coords * sizeof (gdouble));
      *closed = return_vals[3].data.d_int32;
    }

  gimp_destroy_params (return_vals, nreturn_vals);

  return coords;
}

/**
 * gimp_vectors_bezier_stroke_new_moveto:
 * @vectors_ID: The vectors object.
 * @x0: The x-coordinate of the moveto.
 * @y0: The y-coordinate of the moveto.
 *
 * Adds a bezier stroke with a single moveto to the vectors object.
 *
 * Adds a bezier stroke with a single moveto to the vectors object.
 *
 * Returns: The resulting stroke.
 *
 * Since: 2.4
 **/
gint
gimp_vectors_bezier_stroke_new_moveto (gint32  vectors_ID,
                                       gdouble x0,
                                       gdouble y0)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gint stroke_id = 0;

  return_vals = gimp_run_procedure ("gimp-vectors-bezier-stroke-new-moveto",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_FLOAT, x0,
                                    GIMP_PDB_FLOAT, y0,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    stroke_id = return_vals[1].data.d_int32;

  gimp_destroy_params (return_vals, nreturn_vals);

  return stroke_id;
}

/**
 * gimp_vectors_bezier_stroke_lineto:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @x0: The x-coordinate of the lineto.
 * @y0: The y-coordinate of the lineto.
 *
 * Extends a bezier stroke with a lineto.
 *
 * Extends a bezier stroke with a lineto.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_bezier_stroke_lineto (gint32  vectors_ID,
                                   gint    stroke_id,
                                   gdouble x0,
                                   gdouble y0)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-bezier-stroke-lineto",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, x0,
                                    GIMP_PDB_FLOAT, y0,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_bezier_stroke_conicto:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @x0: The x-coordinate of the control point.
 * @y0: The y-coordinate of the control point.
 * @x1: The x-coordinate of the end point.
 * @y1: The y-coordinate of the end point.
 *
 * Extends a bezier stroke with a conic bezier spline.
 *
 * Extends a bezier stroke with a conic bezier spline. Actually a cubic
 * bezier spline gets added that realizes the shape of a conic bezier
 * spline.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_bezier_stroke_conicto (gint32  vectors_ID,
                                    gint    stroke_id,
                                    gdouble x0,
                                    gdouble y0,
                                    gdouble x1,
                                    gdouble y1)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-bezier-stroke-conicto",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, x0,
                                    GIMP_PDB_FLOAT, y0,
                                    GIMP_PDB_FLOAT, x1,
                                    GIMP_PDB_FLOAT, y1,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_bezier_stroke_cubicto:
 * @vectors_ID: The vectors object.
 * @stroke_id: The stroke ID.
 * @x0: The x-coordinate of the first control point.
 * @y0: The y-coordinate of the first control point.
 * @x1: The x-coordinate of the second control point.
 * @y1: The y-coordinate of the second control point.
 * @x2: The x-coordinate of the end point.
 * @y2: The y-coordinate of the end point.
 *
 * Extends a bezier stroke with a cubic bezier spline.
 *
 * Extends a bezier stroke with a cubic bezier spline.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_bezier_stroke_cubicto (gint32  vectors_ID,
                                    gint    stroke_id,
                                    gdouble x0,
                                    gdouble y0,
                                    gdouble x1,
                                    gdouble y1,
                                    gdouble x2,
                                    gdouble y2)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-bezier-stroke-cubicto",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, stroke_id,
                                    GIMP_PDB_FLOAT, x0,
                                    GIMP_PDB_FLOAT, y0,
                                    GIMP_PDB_FLOAT, x1,
                                    GIMP_PDB_FLOAT, y1,
                                    GIMP_PDB_FLOAT, x2,
                                    GIMP_PDB_FLOAT, y2,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_bezier_stroke_new_ellipse:
 * @vectors_ID: The vectors object.
 * @x0: The x-coordinate of the center.
 * @y0: The y-coordinate of the center.
 * @radius_x: The radius in x direction.
 * @radius_y: The radius in y direction.
 * @angle: The angle the x-axis of the ellipse (radians, counterclockwise).
 *
 * Adds a bezier stroke describing an ellipse the vectors object.
 *
 * Adds a bezier stroke describing an ellipse the vectors object.
 *
 * Returns: The resulting stroke.
 *
 * Since: 2.4
 **/
gint
gimp_vectors_bezier_stroke_new_ellipse (gint32  vectors_ID,
                                        gdouble x0,
                                        gdouble y0,
                                        gdouble radius_x,
                                        gdouble radius_y,
                                        gdouble angle)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gint stroke_id = 0;

  return_vals = gimp_run_procedure ("gimp-vectors-bezier-stroke-new-ellipse",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_FLOAT, x0,
                                    GIMP_PDB_FLOAT, y0,
                                    GIMP_PDB_FLOAT, radius_x,
                                    GIMP_PDB_FLOAT, radius_y,
                                    GIMP_PDB_FLOAT, angle,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    stroke_id = return_vals[1].data.d_int32;

  gimp_destroy_params (return_vals, nreturn_vals);

  return stroke_id;
}

/**
 * gimp_vectors_to_selection:
 * @vectors_ID: The vectors object to render to the selection.
 * @operation: The desired operation with current selection.
 * @antialias: Antialias selection.
 * @feather: Feather selection.
 * @feather_radius_x: Feather radius x.
 * @feather_radius_y: Feather radius y.
 *
 * Deprecated: Use gimp_image_select_item() instead.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_to_selection (gint32         vectors_ID,
                           GimpChannelOps operation,
                           gboolean       antialias,
                           gboolean       feather,
                           gdouble        feather_radius_x,
                           gdouble        feather_radius_y)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-to-selection",
                                    &nreturn_vals,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_INT32, operation,
                                    GIMP_PDB_INT32, antialias,
                                    GIMP_PDB_INT32, feather,
                                    GIMP_PDB_FLOAT, feather_radius_x,
                                    GIMP_PDB_FLOAT, feather_radius_y,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_import_from_file:
 * @image_ID: The image.
 * @filename: The name of the SVG file to import.
 * @merge: Merge paths into a single vectors object.
 * @scale: Scale the SVG to image dimensions.
 * @num_vectors: The number of newly created vectors.
 * @vectors_ids: The list of newly created vectors.
 *
 * Import paths from an SVG file.
 *
 * This procedure imports paths from an SVG file. SVG elements other
 * than paths and basic shapes are ignored.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_import_from_file (gint32        image_ID,
                               const gchar  *filename,
                               gboolean      merge,
                               gboolean      scale,
                               gint         *num_vectors,
                               gint32      **vectors_ids)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-import-from-file",
                                    &nreturn_vals,
                                    GIMP_PDB_IMAGE, image_ID,
                                    GIMP_PDB_STRING, filename,
                                    GIMP_PDB_INT32, merge,
                                    GIMP_PDB_INT32, scale,
                                    GIMP_PDB_END);

  *num_vectors = 0;
  *vectors_ids = NULL;

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  if (success)
    {
      *num_vectors = return_vals[1].data.d_int32;
      *vectors_ids = g_new (gint32, *num_vectors);
      memcpy (*vectors_ids,
              return_vals[2].data.d_int32array,
              *num_vectors * sizeof (gint32));
    }

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_import_from_string:
 * @image_ID: The image.
 * @string: A string that must be a complete and valid SVG document.
 * @length: Number of bytes in string or -1 if the string is NULL terminated.
 * @merge: Merge paths into a single vectors object.
 * @scale: Scale the SVG to image dimensions.
 * @num_vectors: The number of newly created vectors.
 * @vectors_ids: The list of newly created vectors.
 *
 * Import paths from an SVG string.
 *
 * This procedure works like gimp_vectors_import_from_file() but takes
 * a string rather than reading the SVG from a file. This allows you to
 * write scripts that generate SVG and feed it to GIMP.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.4
 **/
gboolean
gimp_vectors_import_from_string (gint32        image_ID,
                                 const gchar  *string,
                                 gint          length,
                                 gboolean      merge,
                                 gboolean      scale,
                                 gint         *num_vectors,
                                 gint32      **vectors_ids)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-import-from-string",
                                    &nreturn_vals,
                                    GIMP_PDB_IMAGE, image_ID,
                                    GIMP_PDB_STRING, string,
                                    GIMP_PDB_INT32, length,
                                    GIMP_PDB_INT32, merge,
                                    GIMP_PDB_INT32, scale,
                                    GIMP_PDB_END);

  *num_vectors = 0;
  *vectors_ids = NULL;

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  if (success)
    {
      *num_vectors = return_vals[1].data.d_int32;
      *vectors_ids = g_new (gint32, *num_vectors);
      memcpy (*vectors_ids,
              return_vals[2].data.d_int32array,
              *num_vectors * sizeof (gint32));
    }

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_export_to_file:
 * @image_ID: The image.
 * @filename: The name of the SVG file to create.
 * @vectors_ID: The vectors object to be saved, or 0 for all in the image.
 *
 * save a path as an SVG file.
 *
 * This procedure creates an SVG file to save a Vectors object, that
 * is, a path. The resulting file can be edited using a vector graphics
 * application, or later reloaded into GIMP. If you pass 0 as the
 * 'vectors' argument, then all paths in the image will be exported.
 *
 * Returns: TRUE on success.
 *
 * Since: 2.6
 **/
gboolean
gimp_vectors_export_to_file (gint32       image_ID,
                             const gchar *filename,
                             gint32       vectors_ID)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gboolean success = TRUE;

  return_vals = gimp_run_procedure ("gimp-vectors-export-to-file",
                                    &nreturn_vals,
                                    GIMP_PDB_IMAGE, image_ID,
                                    GIMP_PDB_STRING, filename,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_END);

  success = return_vals[0].data.d_status == GIMP_PDB_SUCCESS;

  gimp_destroy_params (return_vals, nreturn_vals);

  return success;
}

/**
 * gimp_vectors_export_to_string:
 * @image_ID: The image.
 * @vectors_ID: The vectors object to save, or 0 for all in the image.
 *
 * Save a path as an SVG string.
 *
 * This procedure works like gimp_vectors_export_to_file() but creates
 * a string rather than a file. The contents are a NUL-terminated
 * string that holds a complete XML document. If you pass 0 as the
 * 'vectors' argument, then all paths in the image will be exported.
 *
 * Returns: A string whose contents are a complete SVG document.
 *
 * Since: 2.6
 **/
gchar *
gimp_vectors_export_to_string (gint32 image_ID,
                               gint32 vectors_ID)
{
  GimpParam *return_vals;
  gint nreturn_vals;
  gchar *string = NULL;

  return_vals = gimp_run_procedure ("gimp-vectors-export-to-string",
                                    &nreturn_vals,
                                    GIMP_PDB_IMAGE, image_ID,
                                    GIMP_PDB_VECTORS, vectors_ID,
                                    GIMP_PDB_END);

  if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
    string = g_strdup (return_vals[1].data.d_string);

  gimp_destroy_params (return_vals, nreturn_vals);

  return string;
}
