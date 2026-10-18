// first includes global, second local config.h
// it should either be merged or get
// separate names
#include <config.h>
#include "pv_config.h"
#ifdef FBV_SUPPORT_JPEG

#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#if __cplusplus >= 201103
#include <cmath>
#endif
#include <setjmp.h>

#include <global.h>
#include "pictureviewer.h"
#include "picv_client_server.h"

#undef HAVE_STDLIB_H // -Werror complain
extern "C" {
#include <jpeglib.h>
}

#define MIN(a,b) ((a)>(b)?(b):(a))

struct r_jpeg_error_mgr
{
	struct jpeg_error_mgr pub;
	jmp_buf envbuffer;
};

/* the orientation of the camera from the Exif header, 1 when there is none */
static int exif_orientation(struct jpeg_decompress_struct *ciptr)
{
	for (jpeg_saved_marker_ptr m = ciptr->marker_list; m; m = m->next)
	{
		if (m->marker != JPEG_APP0 + 1 || m->data_length < 16 || memcmp(m->data, "Exif\0\0", 6))
			continue;
		const unsigned char *t = m->data + 6;
		unsigned len = m->data_length - 6;
		bool le = t[0] == 'I';
		if (!(le && t[1] == 'I') && !(!le && t[0] == 'M' && t[1] == 'M'))
			return 1;
		auto u16 = [&](unsigned o) { return le ? t[o] | t[o + 1] << 8 : t[o] << 8 | t[o + 1]; };
		auto u32 = [&](unsigned o) { return le ? (unsigned)t[o] | t[o + 1] << 8 | t[o + 2] << 16 | (unsigned)t[o + 3] << 24
							: (unsigned)t[o] << 24 | t[o + 1] << 16 | t[o + 2] << 8 | t[o + 3]; };
		unsigned ifd = u32(4);
		if (ifd + 2 > len)
			return 1;
		unsigned n = u16(ifd);
		for (unsigned i = 0; i < n; i++)
		{
			unsigned e = ifd + 2 + i * 12;
			if (e + 12 > len)
				return 1;
			if (u16(e) == 0x0112 && u16(e + 2) == 3)
			{
				int o = u16(e + 8);
				return (o >= 1 && o <= 8) ? o : 1;
			}
		}
		return 1;
	}
	return 1;
}

/* a new buffer with the picture turned the way the camera held it */
static unsigned char *exif_transform(unsigned char *src, int w, int h, int o, int *ow, int *oh)
{
	bool swap = o >= 5;
	*ow = swap ? h : w;
	*oh = swap ? w : h;
	unsigned char *dst = (unsigned char *)malloc((size_t)w * h * 3);
	if (!dst)
	{
		*ow = w;
		*oh = h;
		return src;
	}
	for (int y = 0; y < *oh; y++)
	{
		for (int x = 0; x < *ow; x++)
		{
			int sx, sy;
			switch (o)
			{
				case 2: sx = w - 1 - x; sy = y; break;
				case 3: sx = w - 1 - x; sy = h - 1 - y; break;
				case 4: sx = x; sy = h - 1 - y; break;
				case 5: sx = y; sy = x; break;
				case 6: sx = y; sy = h - 1 - x; break;
				case 7: sx = w - 1 - y; sy = h - 1 - x; break;
				case 8: sx = w - 1 - y; sy = x; break;
				default: sx = x; sy = y; break;
			}
			memcpy(dst + ((size_t)y * *ow + x) * 3, src + ((size_t)sy * w + sx) * 3, 3);
		}
	}
	free(src);
	return dst;
}


int fh_jpeg_id(const char *name)
{
//	dbout("fh_jpeg_id {\n");
	int fd;
	unsigned char id[10];
	fd=open(name,O_RDONLY); if(fd==-1) return(0);
	ssize_t got = read(fd,id,10);
	close(fd);
	if (got != 10)
		return(0);
//	 dbout("fh_jpeg_id }\n");
	if(id[6]=='J' && id[7]=='F' && id[8]=='I' && id[9]=='F')	return(1);
	if(id[0]==0xff && id[1]==0xd8 && id[2]==0xff) return(1);
	return(0);
}


void jpeg_cb_error_exit(j_common_ptr cinfo)
{
//	dbout("jpeg_cd_error_exit {\n");
	struct r_jpeg_error_mgr *mptr;
	mptr=(struct r_jpeg_error_mgr*) cinfo->err;
	(*cinfo->err->output_message) (cinfo);
	longjmp(mptr->envbuffer,1);
//	 dbout("jpeg_cd_error_exit }\n");
}

int fh_jpeg_load(const char *filename,unsigned char **buffer,int* x,int* y)
{
	//dbout("fh_jpeg_load_local (%s/%d/%d) {\n",basename(filename),*x,*y);
	struct jpeg_decompress_struct cinfo;
	struct jpeg_decompress_struct *ciptr;
	struct r_jpeg_error_mgr emgr;
	unsigned char *bp;
	int px,py,c, ix;
	FILE *fh;
	JSAMPLE *lb;

	ciptr=&cinfo;
	if(!(fh=fopen(filename,"rb"))) return(FH_ERROR_FILE);
	ciptr->err=jpeg_std_error(&emgr.pub);
	emgr.pub.error_exit=jpeg_cb_error_exit;
	if(setjmp(emgr.envbuffer)==1)
	{
		// FATAL ERROR - Free the object and return...
		jpeg_destroy_decompress(ciptr);
		fclose(fh);
//	dbout("fh_jpeg_load } - FATAL ERROR\n");
		return(FH_ERROR_FORMAT);
	}

	jpeg_create_decompress(ciptr);
	jpeg_stdio_src(ciptr,fh);
	jpeg_save_markers(ciptr, JPEG_APP0 + 1, 0xffff);
	jpeg_read_header(ciptr,TRUE);
	ciptr->out_color_space=JCS_RGB;
	ciptr->dct_method=JDCT_FASTEST;
	/* the size asked for is the one of the turned picture */
	int orientation = exif_orientation(ciptr);
	if (orientation >= 5)
	{
		int t = *x; *x = *y; *y = t;
	}
	ix = (int)ciptr->image_width;
	if(*x == ix)
		ciptr->scale_denom=1;
#if __cplusplus < 201103
	else if (abs(*x*2 - ix) < 2)
		ciptr->scale_denom=2;
	else if (abs(*x*4 - ix) < 4)
		ciptr->scale_denom=4;
	else if (abs(*x*8 - ix) < 8)
		ciptr->scale_denom=8;
#else
	else if (std::abs(*x*2 - ix) < 2)
		ciptr->scale_denom=2;
	else if (std::abs(*x*4 - ix) < 4)
		ciptr->scale_denom=4;
	else if (std::abs(*x*8 - ix) < 8)
		ciptr->scale_denom=8;
#endif
	else
		ciptr->scale_denom=1;

	jpeg_start_decompress(ciptr);

	px=ciptr->output_width; py=ciptr->output_height;
	c=ciptr->output_components;
	if(px > *x || py > *y)
	{
		// pic act larger, e.g. because of not responding jpeg server
		free(*buffer);
		*buffer = (px <= INT_MAX / 3 / py) ? (unsigned char*) malloc((size_t)px*py*3) : NULL;
		*x = px;
		*y = py;
		if(*buffer == NULL)
		{
			jpeg_destroy_decompress(ciptr);
			fclose(fh);
			return(FH_ERROR_FORMAT);
		}
	}

	if(c!=3)
	{
		/* out_color_space is RGB, anything else does not fit the 24 bit buffer */
		jpeg_destroy_decompress(ciptr);
		fclose(fh);
		return(FH_ERROR_FORMAT);
	}

	if(c==3)
	{
		lb=(JSAMPLE*)(*ciptr->mem->alloc_small)((j_common_ptr) ciptr,JPOOL_PERMANENT,c*px);
		bp=*buffer;
		while(ciptr->output_scanline < ciptr->output_height)
		{
			jpeg_read_scanlines(ciptr, &lb, 1);
			memmove(bp,lb,px*c);
			bp+=px*c;
		}                 

	}
	jpeg_finish_decompress(ciptr);
	jpeg_destroy_decompress(ciptr);
	fclose(fh);
	if (orientation > 1)
	{
		*buffer = exif_transform(*buffer, px, py, orientation, x, y);
		if (orientation < 5)
		{
			*x = px;
			*y = py;
		}
	}
	//dbout("fh_jpeg_load_local }\n");
	return(FH_ERROR_OK);
}

int fh_jpeg_getsize(const char *filename,int *x,int *y, int wanted_width, int wanted_height)
{
//	dbout("fh_jpeg_getsize {\n");
	struct jpeg_decompress_struct cinfo;
	struct r_jpeg_error_mgr emgr;

	FILE *fh;
	if(!(fh=fopen(filename,"rb"))) return(FH_ERROR_FILE);

	cinfo.err=jpeg_std_error(&emgr.pub);
	emgr.pub.error_exit=jpeg_cb_error_exit;
	if(setjmp(emgr.envbuffer)==1)
	{
		// FATAL ERROR - Free the object and return...
		jpeg_destroy_decompress(&cinfo);
		fclose(fh);
//	dbout("fh_jpeg_getsize } - FATAL ERROR\n");
		return(FH_ERROR_FORMAT);
	}

	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo,fh);
	jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xffff);
	jpeg_read_header(&cinfo,TRUE);
	cinfo.out_color_space=JCS_RGB;
	int orientation = exif_orientation(&cinfo);
	if (orientation >= 5)
	{
		int t = wanted_width; wanted_width = wanted_height; wanted_height = t;
	}
	// should be more flexible...
	if((int)cinfo.image_width/8 >= wanted_width ||
      (int)cinfo.image_height/8 >= wanted_height)
		cinfo.scale_denom=8;
	else if((int)cinfo.image_width/4 >= wanted_width ||
      (int)cinfo.image_height/4 >= wanted_height)
		cinfo.scale_denom=4;
	else if((int)cinfo.image_width/2 >= wanted_width ||
           (int)cinfo.image_height/2 >= wanted_height)
		cinfo.scale_denom=2;
	else
		cinfo.scale_denom=1;

	jpeg_start_decompress(&cinfo);
	*x=cinfo.output_width; *y=cinfo.output_height;
	if (orientation >= 5)
	{
		int t = *x; *x = *y; *y = t;
	}
	jpeg_destroy_decompress(&cinfo);
	fclose(fh);
//	 dbout("fh_jpeg_getsize }\n");
	return(FH_ERROR_OK);
}
#endif
