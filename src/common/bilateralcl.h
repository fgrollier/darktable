/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_COMMON_BILATERAL_CL_H
#define DT_COMMON_BILATERAL_CL_H

#ifdef HAVE_OPENCL
#include "common/opencl.h"

typedef struct dt_bilateral_cl_global_t
{
  int kernel_zero, kernel_splat, kernel_blur_line, kernel_blur_line_z, kernel_slice, kernel_slice2;
}
dt_bilateral_cl_global_t;

typedef struct dt_bilateral_cl_t
{
  dt_bilateral_cl_global_t *global;
  int devid;
  int size_x, size_y, size_z;
  int width, height;
  int blocksizex, blocksizey;
  float sigma_s, sigma_r;
  void *dev_grid;
}
dt_bilateral_cl_t;

dt_bilateral_cl_global_t *
dt_bilateral_init_cl_global()
{
  dt_bilateral_cl_global_t *b = (dt_bilateral_cl_global_t *)malloc(sizeof(dt_bilateral_cl_global_t));

  const int program = 10; // bilateral.cl, from programs.conf
  b->kernel_zero        = dt_opencl_create_kernel(program, "zero");
  b->kernel_splat       = dt_opencl_create_kernel(program, "splat");
  b->kernel_blur_line   = dt_opencl_create_kernel(program, "blur_line");
  b->kernel_blur_line_z = dt_opencl_create_kernel(program, "blur_line_z");
  b->kernel_slice       = dt_opencl_create_kernel(program, "slice");
  b->kernel_slice2      = dt_opencl_create_kernel(program, "slice_to_output");
  return b;
}

void
dt_bilateral_free_cl(
  dt_bilateral_cl_t *b)
{
  if(!b) return;
  // be sure we're done with the memory:
  dt_opencl_finish(b->devid);
  // free device mem
  if(b) dt_opencl_release_mem_object(b->dev_grid);
  free(b);
}

dt_bilateral_cl_t *
dt_bilateral_init_cl(
  const int devid,
  const int width,       // width of input image
  const int height,      // height of input image
  const float sigma_s,   // spatial sigma (blur pixel coords)
  const float sigma_r)   // range sigma (blur luma values)
{
  // check if our device offers enough room for local buffers
  size_t maxsizes[3] = { 0 };        // the maximum dimensions for a work group
  size_t workgroupsize = 0;          // the maximum number of items in a work group
  unsigned long localmemsize = 0;    // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0;    // the maximum amount of items in work group for this kernel


  int blocksizex = 64;
  int blocksizey = 64;
   
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS &&
     dt_opencl_get_kernel_work_group_size(devid, darktable.opencl->bilateral->kernel_splat, &kernelworkgroupsize) == CL_SUCCESS)
  {
    while(maxsizes[0] < blocksizex || maxsizes[1] < blocksizey || localmemsize < blocksizex*blocksizey*(8*sizeof(float)+sizeof(int))
        || workgroupsize < blocksizex*blocksizey || kernelworkgroupsize < blocksizex*blocksizey)
    {
      if(blocksizex == 1 || blocksizey == 1) break;

      if(blocksizex > blocksizey) blocksizex >>= 1;
      else                        blocksizey >>= 1;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_bilateral] can not identify resource limits for device %d in bilateral grid\n", devid);
    return NULL;
  }

  if(blocksizex * blocksizey < 16*16)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_bilateral] device %d does not offer sufficient resources to run bilateral grid\n", devid);
    return NULL;
  }


  dt_bilateral_cl_t *b = (dt_bilateral_cl_t *)malloc(sizeof(dt_bilateral_cl_t));
  if(!b) return NULL;

  b->global = darktable.opencl->bilateral;
  b->size_x = CLAMPS((int)roundf(width/sigma_s), 4, 900) + 1;
  b->size_y = CLAMPS((int)roundf(height/sigma_s), 4, 900) + 1;
  b->size_z = CLAMPS((int)roundf(100.0f/sigma_r), 4, 50) + 1;
  b->width = width;
  b->height = height;
  b->blocksizex = blocksizex;
  b->blocksizey = blocksizey;
  b->sigma_s = MAX(height/(b->size_y-1.0f), width/(b->size_x-1.0f));
  b->sigma_r = 100.0f/(b->size_z-1.0f);
  b->devid = devid;

  // alloc and zero out a grid:
  b->dev_grid = dt_opencl_alloc_device_buffer(b->devid, b->size_x*b->size_y*b->size_z*sizeof(float));
  if(!b->dev_grid)
  {
    dt_bilateral_free_cl(b);
    return NULL;
  }
  int wd = b->size_x, ht = b->size_y*b->size_z;
  size_t sizes[] = { ROUNDUPWD(wd), ROUNDUPHT(ht), 1};
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_zero, 0, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_zero, 1, sizeof(int), (void *)&wd);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_zero, 2, sizeof(int), (void *)&ht);
  cl_int err = -666;
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_zero, sizes);
  if(err != CL_SUCCESS)
  {
    dt_bilateral_free_cl(b);
    return NULL;
  }

#if 0
  fprintf(stderr, "[bilateral] created grid [%d %d %d]"
          " with sigma (%f %f) (%f %f)\n", b->size_x, b->size_y, b->size_z,
          b->sigma_s, sigma_s, b->sigma_r, sigma_r);
#endif
  return b;
}

cl_int
dt_bilateral_splat_cl(
  dt_bilateral_cl_t *b,
  cl_mem in)
{
  cl_int err = -666;
  size_t sizes[] = { ROUNDUP(b->width, b->blocksizex), ROUNDUP(b->height, b->blocksizey), 1};
  size_t local[] = { b->blocksizex, b->blocksizey, 1 };
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 0, sizeof(cl_mem), (void *)&in);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 1, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 2, sizeof(int), (void *)&b->width);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 3, sizeof(int), (void *)&b->height);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 4, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 5, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 6, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 7, sizeof(float), (void *)&b->sigma_s);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 8, sizeof(float), (void *)&b->sigma_r);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 9, b->blocksizex * b->blocksizey * sizeof(int), NULL);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_splat, 10, b->blocksizex * b->blocksizey * 8 * sizeof(float), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(b->devid, b->global->kernel_splat, sizes, local);
  return err;
}

cl_int
dt_bilateral_blur_cl(
  dt_bilateral_cl_t *b)
{
  cl_int err = -666;
  size_t sizes[3] = { 0, 0, 1};
  sizes[0] = ROUNDUPWD(b->size_z);
  sizes[1] = ROUNDUPHT(b->size_y);
  int stride1, stride2, stride3;
  stride1 = b->size_x*b->size_y;
  stride2 = b->size_x;
  stride3 = 1;
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 0, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 1, sizeof(int), (void *)&stride1);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 2, sizeof(int), (void *)&stride2);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 3, sizeof(int), (void *)&stride3);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 4, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 5, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 6, sizeof(int), (void *)&b->size_x);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_blur_line, sizes);
  if(err != CL_SUCCESS) return err;

  stride1 = b->size_x*b->size_y;
  stride2 = 1;
  stride3 = b->size_x;
  sizes[0] = ROUNDUPWD(b->size_z);
  sizes[1] = ROUNDUPHT(b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 0, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 1, sizeof(int), (void *)&stride1);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 2, sizeof(int), (void *)&stride2);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 3, sizeof(int), (void *)&stride3);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 4, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 5, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line, 6, sizeof(int), (void *)&b->size_y);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_blur_line, sizes);
  if(err != CL_SUCCESS) return err;

  stride1 = 1;
  stride2 = b->size_x;
  stride3 = b->size_x*b->size_y;
  sizes[0] = ROUNDUPWD(b->size_x);
  sizes[1] = ROUNDUPHT(b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line_z, 0, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line_z, 1, sizeof(int), (void *)&stride1);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line_z, 2, sizeof(int), (void *)&stride2);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line_z, 3, sizeof(int), (void *)&stride3);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line_z, 4, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line_z, 5, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_blur_line_z, 6, sizeof(int), (void *)&b->size_z);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_blur_line_z, sizes);
  return err;
}

cl_int
dt_bilateral_slice_to_output_cl(
  dt_bilateral_cl_t *b,
  cl_mem in,
  cl_mem out,
  const float detail)
{
  cl_int err = -666;
  size_t sizes[] = { ROUNDUPWD(b->width), ROUNDUPHT(b->height), 1};
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 0, sizeof(cl_mem), (void *)&in);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 1, sizeof(cl_mem), (void *)&out);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 2, sizeof(cl_mem), (void *)&out);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 3, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 4, sizeof(int), (void *)&b->width);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 5, sizeof(int), (void *)&b->height);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 6, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 7, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 8, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 9, sizeof(float), (void *)&b->sigma_s);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 10, sizeof(float), (void *)&b->sigma_r);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice2, 11, sizeof(float), (void *)&detail);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_slice2, sizes);
  return err;
}

cl_int
dt_bilateral_slice_cl(
  dt_bilateral_cl_t *b,
  cl_mem in,
  cl_mem out,
  const float detail)
{
  cl_int err = -666;
  size_t sizes[] = { ROUNDUPWD(b->width), ROUNDUPHT(b->height), 1};
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 0, sizeof(cl_mem), (void *)&in);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 1, sizeof(cl_mem), (void *)&out);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 2, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 3, sizeof(int), (void *)&b->width);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 4, sizeof(int), (void *)&b->height);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 5, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 6, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 7, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 8, sizeof(float), (void *)&b->sigma_s);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 9, sizeof(float), (void *)&b->sigma_r);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_slice, 10, sizeof(float), (void *)&detail);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_slice, sizes);
  return err;
}

void
dt_bilateral_free_cl_global(
  dt_bilateral_cl_global_t *b)
{
  if(!b) return;
  // destroy kernels
  dt_opencl_free_kernel(b->kernel_zero);
  dt_opencl_free_kernel(b->kernel_splat);
  dt_opencl_free_kernel(b->kernel_blur_line);
  dt_opencl_free_kernel(b->kernel_blur_line_z);
  dt_opencl_free_kernel(b->kernel_slice);
  dt_opencl_free_kernel(b->kernel_slice2);
  free(b);
}

#endif
#endif
