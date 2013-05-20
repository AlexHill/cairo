/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is Chris Wilson
 *
 * Contributor(s):
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *    Piotr Tworek <tworaz666@gmail.com>
 */

#include "cairoint.h"
#include "cairo-directfb.h"

#include "cairo-clip-private.h"
#include "cairo-clip-inline.h"
#include "cairo-compositor-private.h"
#include "cairo-default-context-private.h"
#include "cairo-error-private.h"
#include "cairo-image-surface-inline.h"
#include "cairo-pattern-private.h"
#include "cairo-surface-backend-private.h"
#include "cairo-surface-fallback-private.h"

#include <pixman.h>

#include <directfb.h>
#include <direct/types.h>
#include <direct/debug.h>
#include <direct/memcpy.h>
#include <direct/util.h>

slim_hidden_proto(cairo_directfb_surface_create);

typedef struct _cairo_dfb_surface {
	cairo_image_surface_t	image;

	IDirectFB		*dfb;
	IDirectFBSurface	*dfb_surface;

	unsigned		blit_premultiplied : 1;
	unsigned		clipped : 1;
} cairo_dfb_surface_t;

static cairo_content_t
_directfb_format_to_content (DFBSurfacePixelFormat format)
{
	cairo_content_t content = 0;

	if (DFB_PIXELFORMAT_HAS_ALPHA (format))
		content |= CAIRO_CONTENT_ALPHA;
	if (DFB_COLOR_BITS_PER_PIXEL (format))
		content |= CAIRO_CONTENT_COLOR_ALPHA;

	assert(content);
	return content;
}

static inline pixman_format_code_t
_directfb_to_pixman_format (DFBSurfacePixelFormat format)
{
	switch (format) {
	case DSPF_ARGB1555:  return PIXMAN_a1r5g5b5;
	case DSPF_RGB16:     return PIXMAN_r5g6b5;
	case DSPF_RGB24:     return PIXMAN_r8g8b8;
	case DSPF_RGB32:     return PIXMAN_x8r8g8b8;
	case DSPF_ARGB:      return PIXMAN_a8r8g8b8;
	case DSPF_A8:        return PIXMAN_a8;
	case DSPF_YUY2:      return PIXMAN_yuy2;
	case DSPF_RGB332:    return PIXMAN_r3g3b2;
	case DSPF_YV12:      return PIXMAN_yv12;
	case DSPF_ARGB4444:  return PIXMAN_a4r4g4b4;
	case DSPF_A4:        return PIXMAN_a4;
	case DSPF_RGB444:    return PIXMAN_x4r4g4b4;
	case DSPF_RGB555:    return PIXMAN_x1r5g5b5;
	case DSPF_BGR555:    return PIXMAN_x1b5g5r5;

	case DSPF_UNKNOWN:
	case DSPF_UYVY:
	case DSPF_I420:
	case DSPF_LUT8:
	case DSPF_ALUT44:
	case DSPF_AiRGB:
	case DSPF_A1:
	case DSPF_NV12:
	case DSPF_NV16:
	case DSPF_ARGB2554:
	case DSPF_RGBA4444:
	case DSPF_NV21:
	case DSPF_AYUV:
	case DSPF_ARGB1666:
	case DSPF_ARGB6666:
	case DSPF_RGB18:
	case DSPF_LUT2:
	case DSPF_RGBA5551:
	case DSPF_YUV444P:
	case DSPF_ARGB8565:
	case DSPF_AVYU:
	case DSPF_VYU:
	case DSPF_A1_LSB:
	case DSPF_YV16:
	case DSPF_ABGR:
	case DSPF_RGBAF88871:
	case DSPF_LUT4:
	case DSPF_ALUT8:
	case DSPF_LUT1:
	default:
		return 0;
	}
}

static cairo_surface_t *
_cairo_dfb_surface_create_similar (void            *abstract_src,
				   cairo_content_t  content,
				   int              width,
				   int              height)
{
	cairo_dfb_surface_t *other = abstract_src;
	DFBSurfacePixelFormat format;
	IDirectFBSurface *buffer;
	DFBSurfaceDescription dsc;
	cairo_surface_t *surface;

	if (width <= 0 || height <= 0)
		return _cairo_image_surface_create_with_content (content, width, height);

	switch (content) {
	default:
		ASSERT_NOT_REACHED;
	case CAIRO_CONTENT_COLOR_ALPHA:
		format = DSPF_ARGB;
		break;
	case CAIRO_CONTENT_COLOR:
		format = DSPF_RGB32;
		break;
	case CAIRO_CONTENT_ALPHA:
		format = DSPF_A8;
		break;
	}

	dsc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
	dsc.caps        = DSCAPS_PREMULTIPLIED;
	dsc.width       = width;
	dsc.height      = height;
	dsc.pixelformat = format;

	if (other->dfb->CreateSurface (other->dfb, &dsc, &buffer))
		return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_DEVICE_ERROR));

	surface = cairo_directfb_surface_create (other->dfb, buffer);
	buffer->Release (buffer);

	return surface;
}

static cairo_status_t
_cairo_dfb_surface_finish (void *abstract_surface)
{
	cairo_dfb_surface_t *surface = abstract_surface;

	surface->dfb_surface->Release (surface->dfb_surface);
	return _cairo_image_surface_finish (abstract_surface);
}

static cairo_image_surface_t *
_cairo_dfb_surface_map_to_image (void                        *abstract_surface,
				 const cairo_rectangle_int_t *extents)
{
	cairo_dfb_surface_t *surface = abstract_surface;

	if (surface->image.pixman_image == NULL) {
		IDirectFBSurface *buffer = surface->dfb_surface;
		pixman_image_t *image;
		void *data;
		int pitch;

		if (buffer->Lock (buffer, DSLF_READ | DSLF_WRITE, &data, &pitch))
			return _cairo_image_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));

		image = pixman_image_create_bits (surface->image.pixman_format,
		                                  surface->image.width,
		                                  surface->image.height,
		                                  data, pitch);
		if (image == NULL) {
			buffer->Unlock (buffer);
			return _cairo_image_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));
		}
		_cairo_image_surface_init (&surface->image, image, surface->image.pixman_format);
	}

	return _cairo_image_surface_map_to_image (&surface->image.base, extents);
}

static cairo_int_status_t
_cairo_dfb_surface_unmap_image (void                  *abstract_surface,
				cairo_image_surface_t *image)
{
	cairo_dfb_surface_t *surface = abstract_surface;
	return _cairo_image_surface_unmap_image (&surface->image.base, image);
}

static cairo_status_t
_cairo_dfb_surface_acquire_source_image (void *abstract_surface,
                                         cairo_image_surface_t **image_out,
                                         void **image_extra)
{
#if 0
    cairo_qt_surface_t *qs = (cairo_qt_surface_t *) abstract_surface;

    D(fprintf(stderr, "q[%p] acquire_source_image\n", abstract_surface));

    *image_extra = NULL;

    if (qs->image_equiv) {
        *image_out = (cairo_image_surface_t*)
                     cairo_surface_reference (qs->image_equiv);

        return CAIRO_STATUS_SUCCESS;
    }

    if (qs->pixmap) {
        QImage *qimg = new QImage(qs->pixmap->toImage());
	cairo_surface_t *image;
	cairo_status_t status;

        image = cairo_image_surface_create_for_data (qimg->bits(),
						     _cairo_format_from_qimage_format (qimg->format()),
						     qimg->width(), qimg->height(),
						     qimg->bytesPerLine());

	status = _cairo_user_data_array_set_data (&image->user_data,
						  (const cairo_user_data_key_t *)&_qimg_destroy,
						  qimg,
						  _qimg_destroy);
	if (status) {
	    cairo_surface_destroy (image);
	    return status;
	}

	*image_out = (cairo_image_surface_t *) image;
        return CAIRO_STATUS_SUCCESS;
    }

    return _cairo_error (CAIRO_STATUS_NO_MEMORY);
#endif

    fprintf(stderr, "TODO: %s\n", __func__);
    return CAIRO_INT_STATUS_SUCCESS;
}

static void
_cairo_dfb_surface_release_source_image (void *abstract_surface,
                                         cairo_image_surface_t *image,
                                         void *image_extra)
{
#if 0
    //cairo_qt_surface_t *qs = (cairo_qt_surface_t *) abstract_surface;

    D(fprintf(stderr, "q[%p] release_source_image\n", abstract_surface));

    cairo_surface_destroy (&image->base);
#endif
    fprintf(stderr, "TODO: %s\n", __func__);
}

static cairo_status_t
_cairo_dfb_surface_flush (void     *abstract_surface,
			  unsigned flags)
{
	cairo_dfb_surface_t *surface = abstract_surface;

	if (flags)
		return CAIRO_STATUS_SUCCESS;

	if (surface->image.pixman_image) {
		surface->dfb_surface->Unlock (surface->dfb_surface);
		pixman_image_unref (surface->image.pixman_image);
		surface->image.pixman_image = NULL;
		surface->image.data = NULL;
	}

	return CAIRO_STATUS_SUCCESS;
}

#if 0
static cairo_status_t
_cairo_dfb_path_move_to (void *closure, const cairo_point_t *point)
{
	double x = _cairo_fixed_to_double (point->x);
	double y = _cairo_fixed_to_double (point->y);

	fprintf(stderr, "  -> Move To: (%f, %f)\n", x, y);

	return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_dfb_path_line_to (void *closure, const cairo_point_t *point)
{
	double x = _cairo_fixed_to_double (point->x);
	double y = _cairo_fixed_to_double (point->y);

	fprintf(stderr, "  -> Line To: (%f, %f)\n", x, y);

	return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_dfb_path_curve_to (void *closure,
                          const cairo_point_t *p0,
			  const cairo_point_t *p1,
			  const cairo_point_t *p2)
{
	double x0 = _cairo_fixed_to_double (p0->x);
	double y0 = _cairo_fixed_to_double (p0->y);
	double x1 = _cairo_fixed_to_double (p1->x);
	double y1 = _cairo_fixed_to_double (p1->y);
	double x2 = _cairo_fixed_to_double (p2->x);
	double y2 = _cairo_fixed_to_double (p2->y);

	fprintf(stderr, "  -> Curve To: (%f, %f), (%f, %f), (%f, %f)\n", x0, y0, x1, y1, x2, y2);

	return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_dfb_path_close (void *closure)
{

	return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_dfb_path_print(const cairo_path_fixed_t *path)
{
	cairo_status_t status;

	fprintf(stderr, "<Begin path>\n");
	status = _cairo_path_fixed_interpret (path,
	                                      _cairo_dfb_path_move_to,
	                                      _cairo_dfb_path_line_to,
	                                      _cairo_dfb_path_curve_to,
	                                      _cairo_dfb_path_close,
					      NULL);
	fprintf(stderr, "<Close path>\n");

	return status;
}

static inline DFBSurfacePixelFormat
_directfb_from_pixman_format (pixman_format_code_t format)
{
    switch ((int) format) {
    case PIXMAN_a1r5g5b5: return DSPF_ARGB1555;
    case PIXMAN_r5g6b5: return DSPF_RGB16;
    case PIXMAN_r8g8b8: return DSPF_RGB24;
    case PIXMAN_x8r8g8b8: return DSPF_RGB32;
    case PIXMAN_a8r8g8b8: return DSPF_ARGB;
    case PIXMAN_a8: return DSPF_A8;
    case PIXMAN_yuy2: return DSPF_YUY2;
    case PIXMAN_r3g3b2: return DSPF_RGB332;
    case PIXMAN_yv12: return DSPF_YV12;
    case PIXMAN_a1: return DSPF_A1; /* bit reversed, oops */
    case PIXMAN_a4r4g4b4: return DSPF_ARGB4444;
    case PIXMAN_a4: return DSPF_A4;
    case PIXMAN_x4r4g4b4: return DSPF_RGB444;
    case PIXMAN_x1r5g5b5: return DSPF_RGB555;
#if DFB_NUM_PIXELFORMATS >= 29
    case PIXMAN_x1b5g5r5: return DSPF_BGR555;
#endif
    default: return 0;
    }
}
#endif

static cairo_bool_t
_cairo_dfb_is_op_supported (cairo_operator_t operator)
{
	switch (operator) {
	case CAIRO_OPERATOR_CLEAR:
	case CAIRO_OPERATOR_SOURCE:
	case CAIRO_OPERATOR_OVER:
	case CAIRO_OPERATOR_IN:
	case CAIRO_OPERATOR_OUT:
	case CAIRO_OPERATOR_ATOP:
	case CAIRO_OPERATOR_DEST:
	case CAIRO_OPERATOR_DEST_OVER:
	case CAIRO_OPERATOR_DEST_IN:
	case CAIRO_OPERATOR_DEST_OUT:
	case CAIRO_OPERATOR_DEST_ATOP:
	case CAIRO_OPERATOR_XOR:
	case CAIRO_OPERATOR_ADD:
		return TRUE;

	case CAIRO_OPERATOR_SATURATE:
	case CAIRO_OPERATOR_MULTIPLY:
	case CAIRO_OPERATOR_SCREEN:
	case CAIRO_OPERATOR_OVERLAY:
	case CAIRO_OPERATOR_DARKEN:
	case CAIRO_OPERATOR_LIGHTEN:
	case CAIRO_OPERATOR_COLOR_DODGE:
	case CAIRO_OPERATOR_COLOR_BURN:
	case CAIRO_OPERATOR_HARD_LIGHT:
	case CAIRO_OPERATOR_SOFT_LIGHT:
	case CAIRO_OPERATOR_DIFFERENCE:
	case CAIRO_OPERATOR_EXCLUSION:
	case CAIRO_OPERATOR_HSL_HUE:
	case CAIRO_OPERATOR_HSL_SATURATION:
	case CAIRO_OPERATOR_HSL_COLOR:
	case CAIRO_OPERATOR_HSL_LUMINOSITY:
	default:
		fprintf(stderr, "Unsupported cairo operator: %d\n", operator);
		return FALSE;
	}
}

static DFBSurfacePorterDuffRule
_cairo_operator_to_dfb_porter_duff (cairo_operator_t operator)
{
	switch (operator) {
	case CAIRO_OPERATOR_CLEAR:
		return DSPD_CLEAR;
	case CAIRO_OPERATOR_SOURCE:
		return DSPD_SRC;
	case CAIRO_OPERATOR_OVER:
		return DSPD_SRC_OVER;
	case CAIRO_OPERATOR_IN:
		return DSPD_SRC_IN;
	case CAIRO_OPERATOR_OUT:
		return DSPD_SRC_OUT;
	case CAIRO_OPERATOR_ATOP:
		return DSPD_SRC_ATOP;
	case CAIRO_OPERATOR_DEST:
		return DSPD_DST;
	case CAIRO_OPERATOR_DEST_OVER:
		return DSPD_DST_OVER;
	case CAIRO_OPERATOR_DEST_IN:
		return DSPD_DST_IN;
	case CAIRO_OPERATOR_DEST_OUT:
		return DSPD_DST_OUT;
	case CAIRO_OPERATOR_DEST_ATOP:
		return DSPD_DST_ATOP;
	case CAIRO_OPERATOR_XOR:
		return DSPD_XOR;
	case CAIRO_OPERATOR_ADD:
		return DSPD_ADD;

	case CAIRO_OPERATOR_SATURATE:
	case CAIRO_OPERATOR_MULTIPLY:
	case CAIRO_OPERATOR_SCREEN:
	case CAIRO_OPERATOR_OVERLAY:
	case CAIRO_OPERATOR_DARKEN:
	case CAIRO_OPERATOR_LIGHTEN:
	case CAIRO_OPERATOR_COLOR_DODGE:
	case CAIRO_OPERATOR_COLOR_BURN:
	case CAIRO_OPERATOR_HARD_LIGHT:
	case CAIRO_OPERATOR_SOFT_LIGHT:
	case CAIRO_OPERATOR_DIFFERENCE:
	case CAIRO_OPERATOR_EXCLUSION:
	case CAIRO_OPERATOR_HSL_HUE:
	case CAIRO_OPERATOR_HSL_SATURATION:
	case CAIRO_OPERATOR_HSL_COLOR:
	case CAIRO_OPERATOR_HSL_LUMINOSITY:
	default:
		return DSPD_NONE;
	}
}

#if 0
static cairo_bool_t
_directfb_get_operator (cairo_operator_t         operator,
                        cairo_content_t          content,
                        const cairo_pattern_t   *pattern,
                        DFBSurfaceBlendFunction *ret_srcblend,
                        DFBSurfaceBlendFunction *ret_dstblend)
#else
static cairo_bool_t
_directfb_get_operator (cairo_operator_t         operator,
                        DFBSurfaceBlendFunction *ret_srcblend,
                        DFBSurfaceBlendFunction *ret_dstblend)
#endif
{
	DFBSurfaceBlendFunction srcblend = DSBF_ONE;
	DFBSurfaceBlendFunction dstblend = DSBF_ZERO;

	fprintf(stderr, "Operation: %d\n", operator);

	switch (operator) {
	case CAIRO_OPERATOR_CLEAR:
		srcblend = DSBF_ZERO;
		dstblend = DSBF_ZERO;
		break;
	case CAIRO_OPERATOR_SOURCE:
		srcblend = DSBF_ONE;
		dstblend = DSBF_ZERO;
		break;
	case CAIRO_OPERATOR_OVER:
		srcblend = DSBF_ONE;
		dstblend = DSBF_INVSRCALPHA;
		break;
	case CAIRO_OPERATOR_IN:
		srcblend = DSBF_DESTALPHA;
		dstblend = DSBF_ZERO;
		break;
	case CAIRO_OPERATOR_OUT:
		srcblend = DSBF_INVDESTALPHA;
		dstblend = DSBF_ZERO;
		break;
	case CAIRO_OPERATOR_ATOP:
		srcblend = DSBF_DESTALPHA;
		dstblend = DSBF_INVSRCALPHA;
		break;
	case CAIRO_OPERATOR_DEST:
		srcblend = DSBF_ZERO;
		dstblend = DSBF_ONE;
		break;
	case CAIRO_OPERATOR_DEST_OVER:
		srcblend = DSBF_INVDESTALPHA;
		dstblend = DSBF_ONE;
		break;
	case CAIRO_OPERATOR_DEST_IN:
		srcblend = DSBF_ZERO;
		dstblend = DSBF_SRCALPHA;
		break;
	case CAIRO_OPERATOR_DEST_OUT:
		srcblend = DSBF_ZERO;
		dstblend = DSBF_INVSRCALPHA;
		break;
	case CAIRO_OPERATOR_DEST_ATOP:
		srcblend = DSBF_INVDESTALPHA;
		dstblend = DSBF_SRCALPHA;
		break;
	case CAIRO_OPERATOR_XOR:
		srcblend = DSBF_INVDESTALPHA;
		dstblend = DSBF_INVSRCALPHA;
		break;
	case CAIRO_OPERATOR_ADD:
		srcblend = DSBF_ONE;
		dstblend = DSBF_ONE;
		break;
	case CAIRO_OPERATOR_SATURATE:
	/* XXX This does not work. */
#if 0
	srcblend = DSBF_SRCALPHASAT;
	dstblend = DSBF_ONE;
	break;
#endif
	case CAIRO_OPERATOR_MULTIPLY:
	case CAIRO_OPERATOR_SCREEN:
	case CAIRO_OPERATOR_OVERLAY:
	case CAIRO_OPERATOR_DARKEN:
	case CAIRO_OPERATOR_LIGHTEN:
	case CAIRO_OPERATOR_COLOR_DODGE:
	case CAIRO_OPERATOR_COLOR_BURN:
	case CAIRO_OPERATOR_HARD_LIGHT:
	case CAIRO_OPERATOR_SOFT_LIGHT:
	case CAIRO_OPERATOR_DIFFERENCE:
	case CAIRO_OPERATOR_EXCLUSION:
	case CAIRO_OPERATOR_HSL_HUE:
	case CAIRO_OPERATOR_HSL_SATURATION:
	case CAIRO_OPERATOR_HSL_COLOR:
	case CAIRO_OPERATOR_HSL_LUMINOSITY:
	default:
		return FALSE;
	}
#if 0
	if (pattern->type == CAIRO_PATTERN_TYPE_SOLID) {
		const cairo_solid_pattern_t *solid = (cairo_solid_pattern_t *)(pattern);

		if (CAIRO_COLOR_IS_OPAQUE ((&solid->color))) {
			if (srcblend == DSBF_SRCALPHA)
				srcblend = DSBF_ONE;
			else if (srcblend == DSBF_INVSRCALPHA)
				srcblend = DSBF_ZERO;

			if (dstblend == DSBF_SRCALPHA)
				dstblend = DSBF_ONE;
			else if (dstblend == DSBF_INVSRCALPHA)
				dstblend = DSBF_ZERO;
		}
	}

	if ((content & CAIRO_CONTENT_ALPHA) == 0) {
		if (srcblend == DSBF_DESTALPHA)
			srcblend = DSBF_ONE;
		else if (srcblend == DSBF_INVDESTALPHA)
			srcblend = DSBF_ZERO;

		if (dstblend == DSBF_DESTALPHA)
			dstblend = DSBF_ONE;
		else if (dstblend == DSBF_INVDESTALPHA)
			dstblend = DSBF_ZERO;
	}
#endif
	*ret_srcblend = srcblend;
	*ret_dstblend = dstblend;

	return TRUE;
}

static DFBSurfacePixelFormat
_dfb_format_from_cairo_format (cairo_format_t fmt)
{
	switch(fmt) {
	case CAIRO_FORMAT_INVALID:
	case CAIRO_FORMAT_RGB30:
		ASSERT_NOT_REACHED;
	case CAIRO_FORMAT_ARGB32:
		return DSPF_ARGB;
	case CAIRO_FORMAT_RGB24:
		return DSPF_RGB24;
	case CAIRO_FORMAT_A8:
		return DSPF_A8;
	case CAIRO_FORMAT_A1:
		return DSPF_A1;
	case CAIRO_FORMAT_RGB16_565:
		return DSPF_RGB16;
	}

	return DSPF_A1;
}

static DFBRectangle
_dfb_rect_from_cairo_box (const cairo_box_t box)
{
	DFBRectangle rect;
	rect.x = _cairo_fixed_integer_round(box.p1.x);
	rect.y = _cairo_fixed_integer_round(box.p1.y);
	rect.w = abs(_cairo_fixed_integer_round(box.p2.x) - rect.x);
	rect.h = abs(_cairo_fixed_integer_round(box.p2.y) - rect.y);

	return rect;
}

static DFBRectangle
_dfb_rect_from_cairo_box_translate (const cairo_box_t     box,
                                    const cairo_matrix_t *matrix)
{
	DFBRectangle rect;

	double x1 = _cairo_fixed_to_double (box.p1.x);
	double y1 = _cairo_fixed_to_double (box.p1.y);
	double x2 = _cairo_fixed_to_double (box.p2.x);
	double y2 = _cairo_fixed_to_double (box.p2.y);

	cairo_matrix_transform_point (matrix, &x1, &y1);
	cairo_matrix_transform_point (matrix, &x2, &y2);

	rect.x = x1;
	rect.y = y1;
	rect.w = abs(x2 - x1);
	rect.h = abs(y2 - y1);

	return rect;
}

static cairo_int_status_t
_cairo_dfb_surface_set_clip (cairo_dfb_surface_t *ds,
                             const cairo_clip_t  *clip)
{
	DFBResult result = DFB_OK;

	if (clip == NULL && ds->clipped) {
		ds->clipped = 0;
		result = ds->dfb_surface->SetClip(ds->dfb_surface, NULL);
	} else if (clip && !_cairo_clip_is_all_clipped(clip)) {
		DFBRegion region;
		region.x1 = clip->extents.x;
		region.y1 = clip->extents.y;
		region.x2 = region.x1 + clip->extents.width;
		region.y2 = region.y1 + clip->extents.height;
		result = ds->dfb_surface->SetClip(ds->dfb_surface,  &region);
		ds->clipped = 1;
	}

	if (result == DFB_OK) {
		return CAIRO_INT_STATUS_SUCCESS;
	} else {
		return CAIRO_INT_STATUS_UNSUPPORTED;
	}
}

static cairo_int_status_t
_cairo_dfb_surface_fill_solid (cairo_dfb_surface_t      *ds,
                               cairo_operator_t          op,
                               const cairo_pattern_t    *pattern,
			       const cairo_path_fixed_t *path)
{
	const cairo_solid_pattern_t *solid = (cairo_solid_pattern_t *)(pattern);
	DFBSurfaceBlendFunction sblend, dblend;
	DFBSurfaceDrawingFlags flags;
	DFBRectangle rect;
	u8 r, g, b, a;
	cairo_box_t box;

	if (! _cairo_path_fixed_is_rectangle(path, &box))
		return CAIRO_INT_STATUS_UNSUPPORTED;

	if (! _directfb_get_operator(op, &sblend, &dblend))
		return CAIRO_INT_STATUS_UNSUPPORTED;

	flags = (sblend == DSBF_ONE && dblend == DSBF_ZERO) ? DSDRAW_NOFX : DSDRAW_BLEND;
	ds->dfb_surface->SetDrawingFlags (ds->dfb_surface, flags);
	if (flags & DSDRAW_BLEND) {
		ds->dfb_surface->SetSrcBlendFunction (ds->dfb_surface, sblend);
		ds->dfb_surface->SetDstBlendFunction (ds->dfb_surface, dblend);
	}

	if (CAIRO_COLOR_IS_OPAQUE ((&solid->color))) {
		if (sblend == DSBF_SRCALPHA)
			sblend = DSBF_ONE;
		else if (sblend == DSBF_INVSRCALPHA)
			sblend = DSBF_ZERO;

		if (dblend == DSBF_SRCALPHA)
			dblend = DSBF_ONE;
		else if (dblend == DSBF_INVSRCALPHA)
			dblend = DSBF_ZERO;
	}

#if 0
	if ((content & CAIRO_CONTENT_ALPHA) == 0) {
		if (srcblend == DSBF_DESTALPHA)
			srcblend = DSBF_ONE;
		else if (srcblend == DSBF_INVDESTALPHA)
			srcblend = DSBF_ZERO;

		if (dstblend == DSBF_DESTALPHA)
			dstblend = DSBF_ONE;
		else if (dstblend == DSBF_INVDESTALPHA)
			dstblend = DSBF_ZERO;
	}
#endif

	r = solid->color.red_short;
	g = solid->color.green_short;
	b = solid->color.blue_short;
	a = solid->color.alpha_short;

	rect = _dfb_rect_from_cairo_box (box);

	fprintf(stderr, "%s, (%d, %d, %d, %d) -> (r:%d, g:%d, b:%d, a:%d)\n",
	        __func__, rect.x, rect.y, rect.w, rect.h, r, g, b, a);
	int w, h;
	ds->dfb_surface->GetSize (ds->dfb_surface, &w, &h);
	fprintf(stderr, "Surface size: %d, %d\n", w, h);

	if (DFB_OK != ds->dfb_surface->SetColor (ds->dfb_surface, r, g, b, a))
		fprintf(stderr, "Failed to set color!\n");
	if (DFB_OK != ds->dfb_surface->FillRectangle (ds->dfb_surface, rect.x, rect.y, rect.w, rect.h))
		fprintf(stderr, "Failed to fill rectangle!\n");

	return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_dfb_surface_fill_surface (cairo_dfb_surface_t      *destination,
                                 cairo_operator_t          op,
                                 const cairo_pattern_t    *pattern,
				 const cairo_path_fixed_t *path)
{
	const cairo_surface_pattern_t *spattern = (cairo_surface_pattern_t *)(pattern);
	cairo_surface_t *surface = spattern->surface;
	IDirectFBSurface *dst = destination->dfb_surface;
	cairo_box_t box;

	if (! _cairo_path_fixed_is_rectangle(path, &box))
		return CAIRO_INT_STATUS_UNSUPPORTED;

	if (surface->type == CAIRO_SURFACE_TYPE_DIRECTFB) {
		fprintf(stderr, "%s, CAIRO_SURFACE_TYPE_DIRECTFB\n", __func__);
	} else {
		cairo_image_surface_t *imgsurf = NULL;

		if (surface->type == CAIRO_SURFACE_TYPE_IMAGE) {
			imgsurf = (cairo_image_surface_t *) surface;
		} else {
			void *image_extra;
			if (_cairo_surface_acquire_source_image (surface, &imgsurf, &image_extra) != CAIRO_STATUS_SUCCESS) {
				return CAIRO_INT_STATUS_UNSUPPORTED;
			} else {
				imgsurf = NULL;
			}
		}

		if (imgsurf == NULL || (imgsurf->width * imgsurf->height) == 0)
			return CAIRO_INT_STATUS_UNSUPPORTED;

		DFBRectangle r1 = _dfb_rect_from_cairo_box(box);
		DFBRectangle r2 = _dfb_rect_from_cairo_box_translate(box, &pattern->matrix);

		DFBSurfaceDescription dsc;
		dsc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
		dsc.caps        = DSCAPS_PREMULTIPLIED;
		dsc.width       = imgsurf->width;
		dsc.height      = imgsurf->height;
		dsc.pixelformat = _dfb_format_from_cairo_format (imgsurf->format);

		IDirectFBSurface *buffer = NULL;
		if (DFB_OK != destination->dfb->CreateSurface(destination->dfb, &dsc, &buffer))
		{
			assert(false);
			return CAIRO_INT_STATUS_UNSUPPORTED;
		}

		DFBRectangle rb = {0, 0, imgsurf->width, imgsurf->height};
		buffer->Write(buffer, &rb, imgsurf->data, imgsurf->stride);

		dst->SetDrawingFlags (dst, DSDRAW_BLEND);
		dst->SetPorterDuff(dst, _cairo_operator_to_dfb_porter_duff (op));

		if (pattern->extend == CAIRO_EXTEND_NONE) {
			dst->Blit(dst, buffer, &r2, r1.x, r1.y);
		} else if (pattern->extend == CAIRO_EXTEND_REPEAT) {
			dst->TileBlit(dst, buffer, &r2, r1.x, r1.y);
		} else {
			fprintf(stderr, "TODO: %s, Unsupported extend type: %d\n", __func__, pattern->extend);
		}

		buffer->Release(buffer);
	}

	return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_dfb_surface_fill (void *abstract_surface,
			 cairo_operator_t op,
			 const cairo_pattern_t *pattern,
			 const cairo_path_fixed_t *path,
			 cairo_fill_rule_t fill_rule,
			 double tolerance,
			 cairo_antialias_t antialias,
			 const cairo_clip_t *clip)
{
	cairo_dfb_surface_t *dest = abstract_surface;
	DFBSurfaceBlendFunction sblend, dblend;
	DFBSurfaceDrawingFlags flags;

	/*
	 * TODO: Find out what to do with:
	 * - fill_rule
	 * - tolerance
	 * - antialias
	 */

	if (! _cairo_dfb_is_op_supported (op))
		return _cairo_surface_fallback_fill (abstract_surface, op,
		                                     pattern, path, fill_rule,
		                                     tolerance, antialias, clip);

	cairo_int_status_t status = _cairo_dfb_surface_set_clip (dest, clip);
	if (unlikely (status))
		return status;

	switch (pattern->type) {

	case CAIRO_PATTERN_TYPE_SOLID:
		status = _cairo_dfb_surface_fill_solid (dest, op, pattern, path);
		break;

	case CAIRO_PATTERN_TYPE_SURFACE:
		status = _cairo_dfb_surface_fill_surface (dest, op, pattern, path);
		break;

	case CAIRO_PATTERN_TYPE_LINEAR:
		fprintf(stderr, "TODO: %s, Pattern LINEAR\n", __func__);
		break;
	case CAIRO_PATTERN_TYPE_RADIAL:
		fprintf(stderr, "TODO: %s, Pattern RADIAL\n", __func__);
		break;
	case CAIRO_PATTERN_TYPE_MESH:
		fprintf(stderr, "TODO: %s, Pattern MESH\n", __func__);
		break;
	case CAIRO_PATTERN_TYPE_RASTER_SOURCE:
		fprintf(stderr, "TODO: %s, Pattern RASTER_SOURCE\n", __func__);
		break;
	default:
		ASSERT_NOT_REACHED;
	}

	if (status == CAIRO_INT_STATUS_UNSUPPORTED) {
		return _cairo_surface_fallback_fill (abstract_surface, op,
		                                     pattern, path, fill_rule,
		                                     tolerance, antialias, clip);
	} else {
		return status;
	}
}

static cairo_int_status_t
_cairo_dfb_surface_show_glyphs (void *abstract_surface,
			        cairo_operator_t op,
			        const cairo_pattern_t *source,
			        cairo_glyph_t *glyphs,
			        int num_glyphs,
			        cairo_scaled_font_t *scaled_font,
			        const cairo_clip_t *clip)
{
#if 0
	cairo_dfb_surface_t *ds = (cairo_dfb_surface_t *) abstract_surface;
	IDirectFBSurface *dest = ds->dfb_surface;
	IDirectFB *dfb = ds->dfb;
	int currentGlyph;

	cairo_solid_pattern_t *solid = (cairo_solid_pattern_t *) source;

	IDirectFBFont *font;
	DFBFontDescription fontDesc;

	_cairo_scaled_font_freeze_cache(scaled_font);

	FT_Face face = cairo_ft_scaled_font_lock_face (scaled_font);
	const FT_Size_Metrics ftMetrics = face->size->metrics;

	fontDesc.flags = DFDESC_ATTRIBUTES | DFDESC_HEIGHT | DFDESC_WIDTH;
	fontDesc.width = ftMetrics.x_ppem;
	fontDesc.height = ftMetrics.y_ppem;
	fontDesc.attributes = DFFA_NOCHARMAP;

	if (face->style_flags & FT_STYLE_FLAG_BOLD)
		fontDesc.attributes |= DFFA_STYLE_BOLD;
	if (face->style_flags & FT_STYLE_FLAG_ITALIC)
		fontDesc.attributes |= DFFA_STYLE_ITALIC;
	if (!(face->face_flags & FT_FACE_FLAG_KERNING))
		fontDesc.attributes |= DFFA_NOKERNING;

	cairo_ft_scaled_font_unlock_face (scaled_font);

	if (DFB_OK != dfb->CreateFont(dfb, "/usr/share/fonts/dejavu/DejaVuSans.ttf", &fontDesc, &font))
		fprintf(stderr, "Failed to create font!\n");

	dest->SetFont(dest, font);
	dest->SetColor(dest, solid->color.red * 255, solid->color.green * 255,
	               solid->color.blue * 255, solid->color.alpha * 255);

	for (currentGlyph = 0; currentGlyph < num_glyphs; currentGlyph++) {
		dest->DrawGlyph(dest, glyphs[currentGlyph].index, glyphs[currentGlyph].x,
		                glyphs[currentGlyph].y, DSTF_LEFT);
	}

	//fprintf(stderr, "Font family: %s, w=%d, h=%d\n", face->family_name, fontDesc.width, fontDesc.height);

	_cairo_scaled_font_thaw_cache(scaled_font);

	return CAIRO_INT_STATUS_SUCCESS;
#else
	return _cairo_surface_fallback_glyphs (abstract_surface, op,
	                                       source, glyphs, num_glyphs,
	                                       scaled_font, clip);
#endif
}

static cairo_surface_backend_t
_cairo_dfb_surface_backend = {
	CAIRO_SURFACE_TYPE_DIRECTFB, /*type*/
	_cairo_dfb_surface_finish, /*finish*/
	_cairo_default_context_create,

	_cairo_dfb_surface_create_similar,/*create_similar*/
	NULL, /* create similar image */
	_cairo_dfb_surface_map_to_image,
	_cairo_dfb_surface_unmap_image,

	_cairo_surface_default_source,
	_cairo_surface_default_acquire_source_image,
	_cairo_surface_default_release_source_image,
	NULL, /* snapshot */

	NULL, /* copy_page */
	NULL, /* show_page */

	_cairo_image_surface_get_extents,
	_cairo_image_surface_get_font_options,

	_cairo_dfb_surface_flush,
	NULL, /* mark_dirty_rectangle */

	_cairo_surface_fallback_paint,
	_cairo_surface_fallback_mask,
	_cairo_surface_fallback_stroke,
	_cairo_dfb_surface_fill,
	NULL, /* fill-stroke */
	_cairo_dfb_surface_show_glyphs
};

cairo_surface_t *
cairo_directfb_surface_create (IDirectFB *dfb, IDirectFBSurface *dfbsurface)
{
	cairo_dfb_surface_t *surface;
	DFBSurfacePixelFormat     format;
	DFBSurfaceCapabilities caps;
	pixman_format_code_t pixman_format;
	int width, height;

	D_ASSERT (dfb != NULL);
	D_ASSERT (dfbsurface != NULL);

	dfbsurface->GetPixelFormat (dfbsurface, &format);
	dfbsurface->GetSize (dfbsurface, &width, &height);

	pixman_format = _directfb_to_pixman_format (format);
	if (! pixman_format_supported_destination (pixman_format))
		return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_INVALID_FORMAT));

	surface = calloc (1, sizeof (cairo_dfb_surface_t));
	if (surface == NULL)
		return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));

	/* XXX dfb -> device */
	_cairo_surface_init (&surface->image.base,
	                     &_cairo_dfb_surface_backend,
	                     NULL, /* device */
	                     _directfb_format_to_content (format));

	surface->image.pixman_format = pixman_format;
	surface->image.format = _cairo_format_from_pixman_format (pixman_format);

	surface->image.width = width;
	surface->image.height = height;
	surface->image.depth = PIXMAN_FORMAT_DEPTH(pixman_format);

	surface->dfb = dfb;
	surface->dfb_surface = dfbsurface;
	dfbsurface->AddRef (dfbsurface);

	dfbsurface->GetCapabilities (dfbsurface, &caps);
	if (caps & DSCAPS_PREMULTIPLIED)
		surface->blit_premultiplied = TRUE;

	return &surface->image.base;
}
slim_hidden_def(cairo_directfb_surface_create);

IDirectFBSurface *
cairo_directfb_surface_get_surface (cairo_surface_t *surface)
{
	cairo_dfb_surface_t *dfb_surface = (cairo_dfb_surface_t *)surface;
	return dfb_surface->dfb_surface;
}
