/*
 Copyright (c) 2003 Daniel Moreno <comac AT comac DOT darktech DOT org>
 Copyright (c) 2012 Loren Merritt

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "hb.h"
#include "hbffmpeg.h"
#include "mpeg2dec/mpeg2.h"

#define HQDN3D_SPATIAL_LUMA_DEFAULT    4.0f
#define HQDN3D_SPATIAL_CHROMA_DEFAULT  3.0f
#define HQDN3D_TEMPORAL_LUMA_DEFAULT   6.0f

#define ABS(A) ( (A) > 0 ? (A) : -(A) )

struct hb_filter_private_s
{
    int              hqdn3d_coef[4][512*16];
    unsigned int   * hqdn3d_line;
    unsigned short * hqdn3d_frame[3];
};

static int hb_denoise_init( hb_filter_object_t * filter,
                            hb_filter_init_t * init );

static int hb_denoise_work( hb_filter_object_t * filter,
                            hb_buffer_t ** buf_in,
                            hb_buffer_t ** buf_out );

static void hb_denoise_close( hb_filter_object_t * filter );

hb_filter_object_t hb_filter_denoise =
{
    .id            = HB_FILTER_DENOISE,
    .enforce_order = 1,
    .name          = "Denoise (hqdn3d)",
    .settings      = NULL,
    .init          = hb_denoise_init,
    .work          = hb_denoise_work,
    .close         = hb_denoise_close,
};

static void hqdn3d_precalc_coef( int * ct,
                                 double dist25 )
{
    int i;
    double gamma, simil, c;

    gamma = log( 0.25 ) / log( 1.0 - dist25/255.0 - 0.00001 );

    for( i = -255*16; i <= 255*16; i++ )
    {
        simil = 1.0 - ABS(i) / (16*255.0);
        c = pow( simil, gamma ) * 65536.0 * (double)i / 16.0;
        ct[16*256+i] = (c<0) ? (c-0.5) : (c+0.5);
    }

    ct[0] = (dist25 != 0);
}

static inline unsigned int hqdn3d_lowpass_mul( unsigned int prev_mul,
                                               unsigned int curr_mul,
                                               int * coef )
{
    int diff_mul = prev_mul - curr_mul;
    int d = ((diff_mul+0x10007FF)>>12);
    return curr_mul + coef[d];
}

static void hqdn3d_denoise_temporal( unsigned char * frame_src,
                                     unsigned char * frame_dst,
                                     unsigned short * frame_ant,
                                     int w, int h,
                                     int * temporal)
{
    int x, y;
    unsigned int tmp;

    for( y = 0; y < h; y++ )
    {
        for( x = 0; x < w; x++ )
        {
            tmp = hqdn3d_lowpass_mul( frame_ant[x]<<8,
                                      frame_src[x]<<16,
                                      temporal );
            frame_ant[x] = (tmp+0x7F)>>8;
            frame_dst[x] = (tmp+0x7FFF)>>16;
        }

        frame_src += w;
        frame_dst += w;
        frame_ant += w;
    }
}

static void hqdn3d_denoise_spatial( unsigned char * frame_src,
                                    unsigned char * frame_dst,
                                    unsigned int * line_ant,
                                    unsigned short * frame_ant,
                                    int w, int h,
                                    int * spatial,
                                    int * temporal )
{
    int x, y;
    int line_offset_src = 0, line_offset_dst = 0;
    unsigned int pixel_ant;
    unsigned int tmp;

    /* First line has no top neighbor. Only left one for each tmp and last frame */
    pixel_ant = frame_src[0]<<16;
    for ( x = 0; x < w; x++)
    {
        line_ant[x] = tmp = pixel_ant = hqdn3d_lowpass_mul( pixel_ant,
                                                            frame_src[x]<<16,
                                                            spatial );
        tmp = hqdn3d_lowpass_mul( frame_ant[x]<<8, tmp, temporal );
        frame_ant[x] = (tmp+0x7F)>>8;
        frame_dst[x] = (tmp+0x7FFF)>>16;
    }

    for( y = 1; y < h; y++ )
    {
        frame_src += w;
        frame_dst += w;
        frame_ant += w;
        pixel_ant = frame_src[0]<<16;
        for ( x = 0; x < w-1; x++ )
        {
            line_ant[x] = tmp = hqdn3d_lowpass_mul( line_ant[x],
                                                    pixel_ant,
                                                    spatial );
            pixel_ant = hqdn3d_lowpass_mul( pixel_ant,
                                            frame_src[x+1]<<16,
                                            spatial );
            tmp = hqdn3d_lowpass_mul( frame_ant[x]<<8,
                                      tmp,
                                      temporal );
            frame_ant[x] = (tmp+0x7F)>>8;
            frame_dst[x] = (tmp+0x7FFF)>>16;
        }
        line_ant[x] = tmp = hqdn3d_lowpass_mul( line_ant[x],
                                                pixel_ant,
                                                spatial );
        tmp = hqdn3d_lowpass_mul( frame_ant[x]<<8,
                                  tmp,
                                  temporal );
        frame_ant[x] = (tmp+0x7F)>>8;
        frame_dst[x] = (tmp+0x7FFF)>>16;
    }
}

static void hqdn3d_denoise( unsigned char * frame_src,
                            unsigned char * frame_dst,
                            unsigned int * line_ant,
                            unsigned short ** frame_ant_ptr,
                            int w,
                            int h,
                            int * spatial,
                            int * temporal )
{
    int x, y;
    unsigned short* frame_ant = (*frame_ant_ptr);

    if( !frame_ant)
    {
        unsigned char * src = frame_src;
        (*frame_ant_ptr) = frame_ant = malloc( w*h*sizeof(unsigned short) );
        for ( y = 0; y < h; y++, frame_src += w, frame_ant += w )
        {
            for( x = 0; x < w; x++ )
            {
                frame_ant[x] = frame_src[x]<<8;
            }
        }
        frame_src = src;
        frame_ant = *frame_ant_ptr;
    }

    /* If no spatial coefficients, do temporal denoise only */
    if( spatial[0] )
    {
        hqdn3d_denoise_spatial( frame_src,
                                frame_dst,
                                line_ant,
                                frame_ant,
                                w, h,
                                spatial,
                                temporal );
    }
    else
    {
        hqdn3d_denoise_temporal( frame_src,
                                 frame_dst,
                                 frame_ant,
                                 w, h,
                                 temporal);
    }
}

static int hb_denoise_init( hb_filter_object_t * filter,
                            hb_filter_init_t * init )
{
    filter->private_data = calloc( sizeof(struct hb_filter_private_s), 1 );
    hb_filter_private_t * pv = filter->private_data;

    double spatial_luma, temporal_luma, spatial_chroma, temporal_chroma;

    if( filter->settings )
    {
        switch( sscanf( filter->settings, "%lf:%lf:%lf:%lf",
                        &spatial_luma, &spatial_chroma,
                        &temporal_luma, &temporal_chroma ) )
        {
            case 0:
                spatial_luma    = HQDN3D_SPATIAL_LUMA_DEFAULT;

                spatial_chroma  = HQDN3D_SPATIAL_CHROMA_DEFAULT;

                temporal_luma   = HQDN3D_TEMPORAL_LUMA_DEFAULT;

                temporal_chroma = temporal_luma *
                                  spatial_chroma / spatial_luma;
                break;

            case 1:
                spatial_chroma = HQDN3D_SPATIAL_CHROMA_DEFAULT *
                                 spatial_luma / HQDN3D_SPATIAL_LUMA_DEFAULT;

                temporal_luma   = HQDN3D_TEMPORAL_LUMA_DEFAULT *
                                  spatial_luma / HQDN3D_SPATIAL_LUMA_DEFAULT;

                temporal_chroma = temporal_luma *
                                  spatial_chroma / spatial_luma;
                break;

            case 2:
                temporal_luma   = HQDN3D_TEMPORAL_LUMA_DEFAULT *
                                  spatial_luma / HQDN3D_SPATIAL_LUMA_DEFAULT;

                temporal_chroma = temporal_luma *
                                  spatial_chroma / spatial_luma;
                break;

            case 3:
                temporal_chroma = temporal_luma *
                                  spatial_chroma / spatial_luma;
                break;
        }
    }

    hqdn3d_precalc_coef( pv->hqdn3d_coef[0], spatial_luma );
    hqdn3d_precalc_coef( pv->hqdn3d_coef[1], temporal_luma );
    hqdn3d_precalc_coef( pv->hqdn3d_coef[2], spatial_chroma );
    hqdn3d_precalc_coef( pv->hqdn3d_coef[3], temporal_chroma );

    return 0;
}

static void hb_denoise_close( hb_filter_object_t * filter )
{
    hb_filter_private_t * pv = filter->private_data;

    if( !pv )
    {
        return;
    }

	if( pv->hqdn3d_line )
    {
        free( pv->hqdn3d_line );
        pv->hqdn3d_line = NULL;
    }
	if( pv->hqdn3d_frame[0] )
    {
        free( pv->hqdn3d_frame[0] );
        pv->hqdn3d_frame[0] = NULL;
    }
	if( pv->hqdn3d_frame[1] )
    {
        free( pv->hqdn3d_frame[1] );
        pv->hqdn3d_frame[1] = NULL;
    }
	if( pv->hqdn3d_frame[2] )
    {
        free( pv->hqdn3d_frame[2] );
        pv->hqdn3d_frame[2] = NULL;
    }

    free( pv );
    filter->private_data = NULL;
}

static int hb_denoise_work( hb_filter_object_t * filter,
                            hb_buffer_t ** buf_in,
                            hb_buffer_t ** buf_out )
{
    hb_filter_private_t * pv = filter->private_data;
    hb_buffer_t * in = *buf_in, * out;

    if ( in->size <= 0 )
    {
        *buf_out = in;
        *buf_in = NULL;
        return HB_FILTER_DONE;
    }

    out = hb_video_buffer_init( in->f.width, in->f.height );

    if( !pv->hqdn3d_line )
    {
        pv->hqdn3d_line = malloc( in->plane[0].stride * sizeof(int) );
    }

    int ret, c;

    for ( c = 0; c < 3; c++ )
    {
        hqdn3d_denoise( in->plane[c].data,
                        out->plane[c].data,
                        pv->hqdn3d_line,
                        &pv->hqdn3d_frame[c],
                        in->plane[c].stride,
                        in->plane[c].height,
                        pv->hqdn3d_coef[c?2:0],
                        pv->hqdn3d_coef[c?3:1] );
    }

    out->s = in->s;
    hb_buffer_move_subs( out, in );

    *buf_out = out;

    return HB_FILTER_OK;
}
