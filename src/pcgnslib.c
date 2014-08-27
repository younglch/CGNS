/*-------------------------------------------------------------------------
This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from
the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not
   be misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.
-------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcgnslib.h"
#include "cgns_header.h"
#include "cgns_io.h"
#include "mpi.h"
#include "hdf5.h"


#define IS_FIXED_SIZE(type) ((type >= CGNS_ENUMV(NODE) && \
                              type <= CGNS_ENUMV(HEXA_27)) || \
                              type == CGNS_ENUMV(PYRA_13) || \
                             (type >= CGNS_ENUMV(BAR_4) && \
                              type <= CGNS_ENUMV(HEXA_64)))

/*#define MULTIDS*/

static int write_to_queue = 0;
static hid_t default_pio_mode = H5FD_MPIO_INDEPENDENT;

extern int cgns_filetype;

/*===== parallel IO functions =============================*/

static int write_data_parallel(hid_t group_id, CGNS_ENUMT(DataType_t) type,
    int ndims, const cgsize_t *rmin, const cgsize_t *rmax, const void *data)
{
    int k;
    hid_t data_id, mem_shape_id, data_shape_id;
    hsize_t start[3], dims[3];
    herr_t herr;
    hid_t type_id, plist_id;

    /* convert from CGNS to HDF5 data type */
    switch (type) {
        case CGNS_ENUMV(Character):
            type_id = H5T_NATIVE_CHAR;
            break;
        case CGNS_ENUMV(Integer):
            type_id = H5T_NATIVE_INT32;
            break;
        case CGNS_ENUMV(LongInteger):
            type_id = H5T_NATIVE_INT64;
            break;
        case CGNS_ENUMV(RealSingle):
            type_id = H5T_NATIVE_FLOAT;
            break;
        case CGNS_ENUMV(RealDouble):
            type_id = H5T_NATIVE_DOUBLE;
            break;
        default:
            cgi_error("unhandled data type %d\n", type);
            return CG_ERROR;
    }

    /* Open the data */
    if ((data_id = H5Dopen2(group_id, " data", H5P_DEFAULT)) < 0) {
        cgi_error("H5Dopen2() failed");
        return CG_ERROR;
    }

    /* Set the start position and size for the data write */
    /* fix dimensions due to Fortran indexing and ordering */
    for (k = 0; k < ndims; k++) {
        start[k] = rmin[ndims-k-1] - 1;
        dims[k] = rmax[ndims-k-1] - start[k];
    }

    /* Create a shape for the data in memory */
    mem_shape_id = H5Screate_simple(ndims, dims, NULL);
    if (mem_shape_id < 0) {
        H5Dclose(data_id);
        cgi_error("H5Screate_simple() failed");
        return CG_ERROR;
    }

    /* Create a shape for the data in the file */
    data_shape_id = H5Dget_space(data_id);
    if (data_shape_id < 0) {
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Dget_space() failed");
        return CG_ERROR;
    }

    /* Select a section of the array in the file */
    herr = H5Sselect_hyperslab(data_shape_id, H5S_SELECT_SET, start,
                               NULL, dims, NULL);
    if (herr < 0) {
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Sselect_hyperslab() failed");
        return CG_ERROR;
    }

    /* Set the access property list for data transfer */
    plist_id = H5Pcreate(H5P_DATASET_XFER);
    if (plist_id < 0) {
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pcreate() failed");
        return CG_ERROR;
    }

    /* Set MPI-IO independent or collective communication */
    herr = H5Pset_dxpl_mpio(plist_id, default_pio_mode);
    if (herr < 0) {
        H5Pclose(plist_id);
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pset_dxpl_mpio() failed");
        return CG_ERROR;
    }

    /* Write the data in parallel I/O */
    herr = H5Dwrite(data_id, type_id, mem_shape_id,
                    data_shape_id, plist_id, data);
    if (herr < 0) {
        cgi_error("H5Dwrite() failed");
#if 0
    } else {
        herr = H5Fflush(data_id, H5F_SCOPE_GLOBAL);
        if (herr < 0) cgi_error("H5Fflush() failed");
#endif
    }

    H5Pclose(plist_id);
    H5Sclose(data_shape_id);
    H5Sclose(mem_shape_id);
    H5Dclose(data_id);

    return herr < 0 ? CG_ERROR : CG_OK;
}

static int write_multi_data_parallel(hid_t fn, H5D_rw_multi_t *multi_info,
    int ndims, const cgsize_t *rmin, const cgsize_t *rmax)
{
  int k, n;
    hid_t data_id, mem_shape_id, data_shape_id;
    hsize_t start[3], dims[3];
    herr_t herr;
    hid_t plist_id;

    /* convert from CGNS to HDF5 data type */
    for (n = 0; n < 3; n++) {
      switch ((CGNS_ENUMT(DataType_t))multi_info[n].mem_type_id) {
      case CGNS_ENUMV(Character):
	multi_info[n].mem_type_id = H5T_NATIVE_CHAR;
	break;
      case CGNS_ENUMV(Integer):
	multi_info[n].mem_type_id = H5T_NATIVE_INT32;
	break;
      case CGNS_ENUMV(LongInteger):
	multi_info[n].mem_type_id = H5T_NATIVE_INT64;
	break;
      case CGNS_ENUMV(RealSingle):
	multi_info[n].mem_type_id = H5T_NATIVE_FLOAT;
	break;
      case CGNS_ENUMV(RealDouble):
	multi_info[n].mem_type_id = H5T_NATIVE_DOUBLE;
	break;
      default:
	cgi_error("unhandled data type %d\n", multi_info[n].mem_type_id);
	return CG_ERROR;
      }
    }

    /* Set the start position and size for the data write */
    /* fix dimensions due to Fortran indexing and ordering */
    for (k = 0; k < ndims; k++) {
        start[k] = rmin[ndims-k-1] - 1;
        dims[k] = rmax[ndims-k-1] - start[k];
    }

    for (k = 0; k < 3; k++) {
	/* Create a shape for the data in memory */
	multi_info[k].mem_space_id = H5Screate_simple(ndims, dims, NULL);
	if (multi_info[k].mem_space_id < 0) {
	  cgi_error("H5Screate_simple() failed");
	  return CG_ERROR;
	}

	/* Open the data */
	if ((multi_info[k].dset_id = H5Dopen2(multi_info[k].dset_id, " data", H5P_DEFAULT)) < 0) {
	  H5Sclose(multi_info[k].mem_space_id); /** needs loop **/
	  cgi_error("H5Dopen2() failed");
	  return CG_ERROR;
	}

    /* Create a shape for the data in the file */
	multi_info[k].dset_space_id = H5Dget_space(multi_info[k].dset_id);
	if (multi_info[k].dset_space_id < 0) {
	  H5Sclose(multi_info[k].mem_space_id);
	  H5Dclose(multi_info[k].dset_id);
	  cgi_error("H5Dget_space() failed");
	  return CG_ERROR;
	}

    /* Select a section of the array in the file */
	herr = H5Sselect_hyperslab(multi_info[k].dset_space_id, H5S_SELECT_SET, start,
				   NULL, dims, NULL);
	if (herr < 0) {
	  H5Sclose(data_shape_id);
	  H5Sclose(mem_shape_id);
	  H5Dclose(data_id);
	  cgi_error("H5Sselect_hyperslab() failed");
	  return CG_ERROR;
	}
    }


    /* Set the access property list for data transfer */
    plist_id = H5Pcreate(H5P_DATASET_XFER);
    if (plist_id < 0) {
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pcreate() failed");
        return CG_ERROR;
    }

    /* Set MPI-IO independent or collective communication */
    herr = H5Pset_dxpl_mpio(plist_id, H5FD_MPIO_COLLECTIVE);
    if (herr < 0) {
        H5Pclose(plist_id);
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pset_dxpl_mpio() failed");
        return CG_ERROR;
    }

    /* Write the data in parallel I/O */

    herr = H5Dwrite_multi(fn, plist_id, 3, multi_info);

    if (herr < 0) {
        cgi_error("H5Dwrite_multi() failed");
#if 0
    } else {
        herr = H5Fflush(data_id, H5F_SCOPE_GLOBAL);
        if (herr < 0) cgi_error("H5Fflush() failed");
#endif
    }

    H5Pclose(plist_id);
    H5Sclose(data_shape_id);
    H5Sclose(mem_shape_id);
    H5Dclose(data_id);

    return herr < 0 ? CG_ERROR : CG_OK;
}

/*---------------------------------------------------------*/

static int read_data_parallel(hid_t group_id, CGNS_ENUMT(DataType_t) type,
    int ndims, const cgsize_t *rmin, const cgsize_t *rmax, void *data)
{
    int k;
    hid_t data_id, mem_shape_id, data_shape_id;
    hsize_t start[3], dims[3];
    herr_t herr;
    hid_t type_id, plist_id;

    /* convert from CGNS to HDF5 data type */
    switch (type) {
        case CGNS_ENUMV(Character):
            type_id = H5T_NATIVE_CHAR;
            break;
        case CGNS_ENUMV(Integer):
            type_id = H5T_NATIVE_INT32;
            break;
        case CGNS_ENUMV(LongInteger):
            type_id = H5T_NATIVE_INT64;
            break;
        case CGNS_ENUMV(RealSingle):
            type_id = H5T_NATIVE_FLOAT;
            break;
        case CGNS_ENUMV(RealDouble):
            type_id = H5T_NATIVE_DOUBLE;
            break;
        default:
            cgi_error("unhandled data type %d\n", type);
            return CG_ERROR;
    }

    /* Open the data */
    if ((data_id = H5Dopen2(group_id, " data", H5P_DEFAULT)) < 0) {
        cgi_error("H5Dopen2() failed");
        return CG_ERROR;
    }

    /* Set the start position and size for the data write */
    /* fix dimensions due to Fortran indexing and ordering */
    for (k = 0; k < ndims; k++) {
        start[k] = rmin[ndims-k-1] - 1;
        dims[k] = rmax[ndims-k-1] - start[k];
    }

    /* Create a shape for the data in memory */
    mem_shape_id = H5Screate_simple(ndims, dims, NULL);
    if (mem_shape_id < 0) {
        H5Dclose(data_id);
        cgi_error("H5Screate_simple() failed");
        return CG_ERROR;
    }

    /* Create a shape for the data in the file */
    data_shape_id = H5Dget_space(data_id);
    if (data_shape_id < 0) {
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Dget_space() failed");
        return CG_ERROR;
    }

    /* Select a section of the array in the file */
    herr = H5Sselect_hyperslab(data_shape_id, H5S_SELECT_SET, start,
                               NULL, dims, NULL);
    if (herr < 0) {
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Sselect_hyperslab() failed");
        return CG_ERROR;
    }

    /* Set the access property list for data transfer */
    plist_id = H5Pcreate(H5P_DATASET_XFER);
    if (plist_id < 0) {
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pcreate() failed");
        return CG_ERROR;
    }

    /* Set MPI-IO independent or collective communication */
    herr = H5Pset_dxpl_mpio(plist_id, default_pio_mode);
    if (herr < 0) {
        H5Pclose(plist_id);
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pset_dxpl_mpio() failed");
        return CG_ERROR;
    }

    /* Write the data in parallel I/O */
    herr = H5Dread(data_id, type_id, mem_shape_id,
                   data_shape_id, plist_id, data);
    if (herr < 0) {
        cgi_error("H5Dread() failed");
#if 0
    } else {
        herr = H5Fflush(data_id, H5F_SCOPE_GLOBAL);
        if (herr < 0) cgi_error("H5Fflush() failed");
#endif
    }

    H5Pclose(plist_id);
    H5Sclose(data_shape_id);
    H5Sclose(mem_shape_id);
    H5Dclose(data_id);

    return herr < 0 ? CG_ERROR : CG_OK;
}

/*===== queued IO functions ===============================*/

typedef struct slice_s {
    hid_t pid;
    CGNS_ENUMT(DataType_t) type;
    int ndims;
    cgsize_t rmin[3];
    cgsize_t rmax[3];
    const void *data;
} slice_t;

static slice_t *write_queue = NULL;
static int write_queue_len = 0;

/*---------------------------------------------------------*/

static int write_data_queue(hid_t pid, CGNS_ENUMT(DataType_t) type,
    int ndims, const cgsize_t *rmin, const cgsize_t *rmax, const void *data)
{
    int n;
    slice_t *slice;

    if (write_queue_len == 0) {
        write_queue = (slice_t *)malloc(sizeof(slice_t));
    }
    else {
        write_queue = (slice_t *)realloc(write_queue,
                          (write_queue_len+1) * sizeof(slice_t));
    }
    slice = &write_queue[write_queue_len++];

    slice->pid = pid;
    slice->type = type;
    slice->ndims = ndims;
    for (n = 0; n < ndims; n++) {
        slice->rmin[n] = rmin[n];
        slice->rmax[n] = rmax[n];
    }
    slice->data = data;
    return CG_OK;
}

/*---------------------------------------------------------*/

static int check_parallel(cgns_file *cgfile)
{
    int type;

    if (cgfile == NULL) return CG_ERROR;
    if (cgio_get_file_type(cgfile->cgio, &type) ||
        type != CGIO_FILE_PHDF5) {
        cgi_error("file not opened for parallel IO");
        return CG_ERROR;
    }
    return CG_OK;
}

#ifdef MULTIDS
static int readwrite_multi_data_parallel(hid_t fn, H5D_rw_multi_t *multi_info,
    int ndims, const cgsize_t *rmin, const cgsize_t *rmax, int rw_mode)
{
    int k, n;
    hid_t data_id, mem_shape_id, data_shape_id;
    hsize_t start[3], dims[3];
    herr_t herr;
    hid_t plist_id;

    /* convert from CGNS to HDF5 data type */
    for (n = 0; n < 3; n++) {
      switch ((CGNS_ENUMT(DataType_t))multi_info[n].mem_type_id) {
      case CGNS_ENUMV(Character):
	multi_info[n].mem_type_id = H5T_NATIVE_CHAR;
	break;
      case CGNS_ENUMV(Integer):
	multi_info[n].mem_type_id = H5T_NATIVE_INT32;
	break;
      case CGNS_ENUMV(LongInteger):
	multi_info[n].mem_type_id = H5T_NATIVE_INT64;
	break;
      case CGNS_ENUMV(RealSingle):
	multi_info[n].mem_type_id = H5T_NATIVE_FLOAT;
	break;
      case CGNS_ENUMV(RealDouble):
	multi_info[n].mem_type_id = H5T_NATIVE_DOUBLE;
	break;
      default:
	cgi_error("unhandled data type %d\n", multi_info[n].mem_type_id);
	return CG_ERROR;
      }
    }

    /* Set the start position and size for the data write */
    /* fix dimensions due to Fortran indexing and ordering */
    for (k = 0; k < ndims; k++) {
        start[k] = rmin[ndims-k-1] - 1;
        dims[k] = rmax[ndims-k-1] - start[k];
    }

    for (k = 0; k < 3; k++) {
	/* Create a shape for the data in memory */
	multi_info[k].mem_space_id = H5Screate_simple(ndims, dims, NULL);
	if (multi_info[k].mem_space_id < 0) {
	  cgi_error("H5Screate_simple() failed");
	  return CG_ERROR;
	}

	/* Open the data */
	if ((multi_info[k].dset_id = H5Dopen2(multi_info[k].dset_id, " data", H5P_DEFAULT)) < 0) {
	  H5Sclose(multi_info[k].mem_space_id); /** needs loop **/
	  cgi_error("H5Dopen2() failed");
	  return CG_ERROR;
	}

    /* Create a shape for the data in the file */
	multi_info[k].dset_space_id = H5Dget_space(multi_info[k].dset_id);
	if (multi_info[k].dset_space_id < 0) {
	  H5Sclose(multi_info[k].mem_space_id);
	  H5Dclose(multi_info[k].dset_id);
	  cgi_error("H5Dget_space() failed");
	  return CG_ERROR;
	}

    /* Select a section of the array in the file */
	herr = H5Sselect_hyperslab(multi_info[k].dset_space_id, H5S_SELECT_SET, start,
				   NULL, dims, NULL);
	if (herr < 0) {
	  H5Sclose(data_shape_id);
	  H5Sclose(mem_shape_id);
	  H5Dclose(data_id);
	  cgi_error("H5Sselect_hyperslab() failed");
	  return CG_ERROR;
	}
    }


    /* Set the access property list for data transfer */
    plist_id = H5Pcreate(H5P_DATASET_XFER);
    if (plist_id < 0) {
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pcreate() failed");
        return CG_ERROR;
    }

    /* Set MPI-IO independent or collective communication */
    herr = H5Pset_dxpl_mpio(plist_id, default_pio_mode);
    if (herr < 0) {
        H5Pclose(plist_id);
        H5Sclose(data_shape_id);
        H5Sclose(mem_shape_id);
        H5Dclose(data_id);
        cgi_error("H5Pset_dxpl_mpio() failed");
        return CG_ERROR;
    }

    /* Write the data in parallel I/O */

    if (rw_mode == 0) {
      herr = H5Dread_multi(fn, plist_id, 3, multi_info);
      if (herr < 0)
        cgi_error("H5Dread_multi() failed");
        return CG_ERROR;
    } else {
      herr = H5Dwrite_multi(fn, plist_id, 3, multi_info);
      if (herr < 0)
        cgi_error("H5Dwrite_multi() failed");
        return CG_ERROR;
    }

    H5Pclose(plist_id);
    H5Sclose(data_shape_id);
    H5Sclose(mem_shape_id);
    H5Dclose(data_id);

    return herr < 0 ? CG_ERROR : CG_OK;
}

#endif

/*================================*/
/*== Begin Function Definitions ==*/
/*================================*/

int cgp_mpi_comm(int comm)
{
    return cgio_configure(CG_CONFIG_HDF5_MPI_COMM, (void *)((size_t)comm));
}

/*---------------------------------------------------------*/

int cgp_pio_mode(CGNS_ENUMT(PIOmode_t) mode, MPI_Info info)
{
    if (mode == CGP_INDEPENDENT)
        default_pio_mode = H5FD_MPIO_INDEPENDENT;
    else if (mode == CGP_COLLECTIVE)
        default_pio_mode = H5FD_MPIO_COLLECTIVE;
    else {
        cgi_error("unknown parallel IO mode");
        return CG_ERROR;
    }

    pcg_mpi_info = info;

    return CG_OK;
}

/*---------------------------------------------------------*/

int cgp_queue_set(int use_queue)
{
    write_to_queue = use_queue;
    return CG_OK;
}

/*---------------------------------------------------------*/

int cgp_queue_flush(void)
{
    int n, errs = 0;

    if (write_queue_len) {
        for (n = 0; n < write_queue_len; n++) {
            if (write_data_parallel(write_queue[n].pid, write_queue[n].type,
                    write_queue[n].ndims, write_queue[n].rmin,
                    write_queue[n].rmax, write_queue[n].data)) errs++;
        }
        free(write_queue);
        write_queue = NULL;
        write_queue_len = 0;
    }
    return errs ? CG_ERROR : CG_OK;
}

/*---------------------------------------------------------*/

void cgp_error_exit(void)
{
    int rank;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    fprintf(stderr, "[process %d]:%s\n", rank, cg_get_error());
    cgio_cleanup();
    MPI_Abort(MPI_COMM_WORLD, 1);
}

/*===== File IO Prototypes ================================*/

int cgp_open(const char *filename, int mode, int *fn)
{
    int ierr, old_type = cgns_filetype;

    ierr = cg_set_file_type(CG_FILE_PHDF5);
    if (ierr) return ierr;
    ierr = cg_open(filename, mode, fn);
    cgns_filetype = old_type;
    return ierr;
}

/*---------------------------------------------------------*/

int cgp_close(int fn)
{
    cgp_queue_flush();
    return cg_close(fn);
}

/*===== Grid IO Prototypes ================================*/

int cgp_coord_write(int fn, int B, int Z, CGNS_ENUMT(DataType_t) type,
    const char *coordname, int *C)
{
    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    return cg_coord_write(fn, B, Z, type, coordname, NULL, C);
}

/*---------------------------------------------------------*/

int cgp_coord_write_data(int fn, int B, int Z, int C,
    const cgsize_t *rmin, const cgsize_t *rmax, const void *coords)
{
    int n;
    cgns_zone *zone;
    cgns_zcoor *zcoor;
    cgsize_t dims[3];
    CGNS_ENUMT(DataType_t) type;

    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_WRITE))
        return CG_ERROR;

    zone = cgi_get_zone(cg, B, Z);
    if (zone==0) return CG_ERROR;

    zcoor = cgi_get_zcoorGC(cg, B, Z);
    if (zcoor==0) return CG_ERROR;

    if (C > zcoor->ncoords || C <= 0) {
        cgi_error("coord number %d invalid",C);
        return CG_ERROR;
    }

    for (n = 0; n < zone->index_dim; n++) {
        dims[n] = zone->nijk[n] + zcoor->rind_planes[2*n] +
                                  zcoor->rind_planes[2*n+1];
        if (rmin[n] > rmax[n] || rmin[n] < 1 || rmax[n] > dims[n]) {
            cgi_error("Invalid index ranges.");
            return CG_ERROR;
        }
    }
    type = cgi_datatype(zcoor->coord[C-1].data_type);

    if (write_to_queue) {
        return write_data_queue((hid_t)zcoor->coord[C-1].id, type,
                   zone->index_dim, rmin, rmax, coords);
    }
    return write_data_parallel((hid_t)zcoor->coord[C-1].id, type,
               zone->index_dim, rmin, rmax, coords);
}

int cgp_coords_write_data(int fn, int B, int Z, int *C,
    const cgsize_t *rmin, const cgsize_t *rmax, const void *coordsX, const void *coordsY, const void *coordsZ)
{
    int n;
    cgns_zone *zone;
    cgns_zcoor *zcoor;
    cgsize_t dims[3];
    cgsize_t index_dim;
    CGNS_ENUMT(DataType_t) type[3];
    H5D_rw_multi_t multi_info[3];

    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_WRITE))
        return CG_ERROR;

    zone = cgi_get_zone(cg, B, Z);
    if (zone==0) return CG_ERROR;

    zcoor = cgi_get_zcoorGC(cg, B, Z);
    if (zcoor==0) return CG_ERROR;

    for (n = 0;  n < 3; n++) {
      if (C[n] > zcoor->ncoords || C[n] <= 0) {
        cgi_error("coord number %d invalid",C[n]);
        return CG_ERROR;
      }
    }

    for (n = 0; n < zone->index_dim; n++) {
        dims[n] = zone->nijk[n] + zcoor->rind_planes[2*n] +
                                  zcoor->rind_planes[2*n+1];
        if (rmin[n] > rmax[n] || rmin[n] < 1 || rmax[n] > dims[n]) {
            cgi_error("Invalid index ranges.");
            return CG_ERROR;
        }
    }
    
    for (n = 0; n < 3; n++) {
      multi_info[n].mem_type_id = cgi_datatype(zcoor->coord[C[n]-1].data_type);
      multi_info[n].dset_id = (hid_t)zcoor->coord[C[n]-1].id;
    }

    multi_info[0].u.wbuf = coordsX;
    multi_info[1].u.wbuf = coordsY;
    multi_info[2].u.wbuf = coordsZ;

    return write_multi_data_parallel(fn, multi_info,
               zone->index_dim, rmin, rmax);
}

/*---------------------------------------------------------*/

int cgp_coord_read_data(int fn, int B, int Z, int C,
    const cgsize_t *rmin, const cgsize_t *rmax, void *coords)
{
    int n;
    cgns_zone *zone;
    cgns_zcoor *zcoor;
    cgsize_t dims[3];
    CGNS_ENUMT(DataType_t) type;

    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_READ))
        return CG_ERROR;

    zone = cgi_get_zone(cg, B, Z);
    if (zone==0) return CG_ERROR;

    zcoor = cgi_get_zcoorGC(cg, B, Z);
    if (zcoor==0) return CG_ERROR;

    if (C > zcoor->ncoords || C <= 0) {
        cgi_error("coord number %d invalid",C);
        return CG_ERROR;
    }

    for (n = 0; n < zone->index_dim; n++) {
        dims[n] = zone->nijk[n] + zcoor->rind_planes[2*n] +
                                  zcoor->rind_planes[2*n+1];
        if (rmin[n] > rmax[n] || rmin[n] < 1 || rmax[n] > dims[n]) {
            cgi_error("Invalid index ranges.");
            return CG_ERROR;
        }
    }
    type = cgi_datatype(zcoor->coord[C-1].data_type);

    return read_data_parallel((hid_t)zcoor->coord[C-1].id, type,
               zone->index_dim, rmin, rmax, coords);
}

/*===== Elements IO Prototypes ============================*/

int cgp_section_write(int fn, int B, int Z, const char *sectionname,
    CGNS_ENUMT(ElementType_t) type, cgsize_t start, cgsize_t end,
    int nbndry, int *S)
{
    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;
    if (!IS_FIXED_SIZE(type)) {
        cgi_error("element must be a fixed size for parallel IO");
        return CG_ERROR;
    }

    return cg_section_partial_write(fn, B, Z, sectionname, type,
               start, end, nbndry, S);
}

/*---------------------------------------------------------*/

int cgp_elements_write_data(int fn, int B, int Z, int S, cgsize_t start,
    cgsize_t end, const cgsize_t *elements)
{
    int elemsize;
    cgns_section *section;
    cgsize_t rmin, rmax;
    CGNS_ENUMT(DataType_t) type;

     /* get file and check mode */
    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_WRITE))
        return CG_ERROR;

    section = cgi_get_section(cg, B, Z, S);
    if (section == 0 || section->connect == 0) return CG_ERROR;

    if (start > end ||
        start < section->range[0] ||
        end > section->range[1]) {
	cgi_error("Error in requested element data range.");
        return CG_ERROR;
    }
    if (!IS_FIXED_SIZE(section->el_type)) {
        cgi_error("element must be a fixed size for parallel IO");
        return CG_ERROR;
    }

    if (cg_npe(section->el_type, &elemsize)) return CG_ERROR;
    rmin = (start - section->range[0]) * elemsize + 1;
    rmax = (end - section->range[0] + 1) * elemsize;
    type = cgi_datatype(section->connect->data_type);

    if (write_to_queue) {
        return write_data_queue((hid_t)section->connect->id, type,
                   1, &rmin, &rmax, elements);
    }
    return write_data_parallel((hid_t)section->connect->id, type,
               1, &rmin, &rmax, elements);
}

/*---------------------------------------------------------*/

int cgp_elements_read_data(int fn, int B, int Z, int S, cgsize_t start,
    cgsize_t end, cgsize_t *elements)
{
    int elemsize;
    cgns_section *section;
    cgsize_t rmin, rmax;
    CGNS_ENUMT(DataType_t) type;

     /* get file and check mode */
    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_READ))
        return CG_ERROR;

    section = cgi_get_section(cg, B, Z, S);
    if (section == 0 || section->connect == 0) return CG_ERROR;

    if (start > end ||
        start < section->range[0] ||
        end > section->range[1]) {
	cgi_error("Error in requested element data range.");
        return CG_ERROR;
    }
    if (!IS_FIXED_SIZE(section->el_type)) {
        cgi_error("element must be a fixed size for parallel IO");
        return CG_ERROR;
    }

    if (cg_npe(section->el_type, &elemsize)) return CG_ERROR;
    rmin = (start - section->range[0]) * elemsize + 1;
    rmax = (end - section->range[0] + 1) * elemsize;
    type = cgi_datatype(section->connect->data_type);

    return read_data_parallel((hid_t)section->connect->id, type,
               1, &rmin, &rmax, elements);
}

/*===== Solution IO Prototypes ============================*/

int cgp_field_write(int fn, int B, int Z, int S,
    CGNS_ENUMT(DataType_t) DataType, const char *fieldname, int *F)
{
    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    return cg_field_write(fn, B, Z, S, DataType, fieldname, NULL, F);
}

/*---------------------------------------------------------*/

int cgp_field_write_data(int fn, int B, int Z, int S, int F,
    const cgsize_t *rmin, const cgsize_t *rmax, const void *data)
{
    int n;
    cgns_array *field;
    CGNS_ENUMT(DataType_t) type;

    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_WRITE))
        return CG_ERROR;

    field = cgi_get_field(cg, B, Z, S, F);
    if (field==0) return CG_ERROR;

     /* verify that range requested does not exceed range stored */
    for (n = 0; n < field->data_dim; n++) {
        if (rmin[n] > rmax[n] ||
            rmax[n] > field->dim_vals[n] ||
            rmin[n] < 1) {
            cgi_error("Invalid range of data requested");
            return CG_ERROR;
        }
    }
    type = cgi_datatype(field->data_type);

    if (write_to_queue) {
        return write_data_queue((hid_t)field->id, type,
                   field->data_dim, rmin, rmax, data);
    }
    return write_data_parallel((hid_t)field->id, type,
               field->data_dim, rmin, rmax, data);
}

/*---------------------------------------------------------*/

int cgp_field_read_data(int fn, int B, int Z, int S, int F,
    const cgsize_t *rmin, const cgsize_t *rmax, void *data)
{
    int n;
    cgns_array *field;
    CGNS_ENUMT(DataType_t) type;

    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_READ))
        return CG_ERROR;

    field = cgi_get_field(cg, B, Z, S, F);
    if (field==0) return CG_ERROR;

     /* verify that range requested does not exceed range stored */
    for (n = 0; n < field->data_dim; n++) {
        if (rmin[n] > rmax[n] ||
            rmax[n] > field->dim_vals[n] ||
            rmin[n] < 1) {
            cgi_error("Invalid range of data requested");
            return CG_ERROR;
        }
    }
    type = cgi_datatype(field->data_type);

    return read_data_parallel((hid_t)field->id, type,
               field->data_dim, rmin, rmax, data);
}

/*===== Array IO Prototypes ===============================*/

int cgp_array_write(const char *ArrayName, CGNS_ENUMT(DataType_t) DataType,
    int DataDimension, const cgsize_t *DimensionVector, int *A)
{
    int ierr, na, n;
    cgns_array *array;

   if (posit == NULL) {
        cgi_error("No current position set by cg_goto");
        return CG_ERROR;
    }
    if (check_parallel(cg)) return CG_ERROR;

    ierr = cg_array_write(ArrayName, DataType, DataDimension,
                          DimensionVector, NULL);
    if (ierr) return ierr;
    array = cgi_array_address(CG_MODE_READ, 1, "dummy", &ierr);
    if (array == NULL) return ierr;
    ierr = cg_narrays(&na);
    if (ierr) return ierr;
    for (n = 0; n < na; n++) {
        if (0 == strcmp(ArrayName, array->name)) {
            *A = n + 1;
            return CG_OK;
        }
        array++;
    }
    *A = 0;
    cgi_error("array %s not found", ArrayName);
    return CG_ERROR;
}

/*---------------------------------------------------------*/

int cgp_array_write_data(int A, const cgsize_t *rmin,
    const cgsize_t *rmax, const void *data)
{
    int n, ierr = 0;
    cgns_array *array;
    CGNS_ENUMT(DataType_t) type;

    array = cgi_array_address(CG_MODE_READ, A, "dummy", &ierr);
    if (array == NULL) return ierr;

    for (n = 0; n < array->data_dim; n++) {
        if (rmin[n] > rmax[n] ||
            rmax[n] > array->dim_vals[n] ||
            rmin[n] < 1) {
            cgi_error("Invalid range of data requested");
            return CG_ERROR;
        }
    }
    type = cgi_datatype(array->data_type);

    if (write_to_queue) {
        return write_data_queue((hid_t)array->id, type,
                   array->data_dim, rmin, rmax, data);
    }
    return write_data_parallel((hid_t)array->id, type,
               array->data_dim, rmin, rmax, data);
}

/*---------------------------------------------------------*/

int cgp_array_read_data(int A, const cgsize_t *rmin,
    const cgsize_t *rmax, void *data)
{
    int n, ierr = 0;
    cgns_array *array;
    CGNS_ENUMT(DataType_t) type;

    array = cgi_array_address(CG_MODE_READ, A, "dummy", &ierr);
    if (array == NULL) return ierr;

    for (n = 0; n < array->data_dim; n++) {
        if (rmin[n] > rmax[n] ||
            rmax[n] > array->dim_vals[n] ||
            rmin[n] < 1) {
            cgi_error("Invalid range of data requested");
            return CG_ERROR;
        }
    }
    type = cgi_datatype(array->data_type);

    return read_data_parallel((hid_t)array->id, type,
               array->data_dim, rmin, rmax, data);
}

#ifdef MULTIDS

/*------------------- multi-dataset functions --------------------------------------*/

int cgp_coords_read_data(int fn, int B, int Z, int *C,
    const cgsize_t *rmin, const cgsize_t *rmax, void *coordsX,  void *coordsY,  void *coordsZ)
{
  int n;
  cgns_zone *zone;
  cgns_zcoor *zcoor;
  cgsize_t dims[3];
  cgsize_t index_dim;
  CGNS_ENUMT(DataType_t) type[3];
  H5D_rw_multi_t multi_info[3];

  cg = cgi_get_file(fn);
  if (check_parallel(cg)) return CG_ERROR;

  if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_WRITE))
    return CG_ERROR;
  
  zone = cgi_get_zone(cg, B, Z);
  if (zone==0) return CG_ERROR;
  
  zcoor = cgi_get_zcoorGC(cg, B, Z);
  if (zcoor==0) return CG_ERROR;
  
  for (n = 0;  n < 3; n++) {
    if (C[n] > zcoor->ncoords || C[n] <= 0) {
      cgi_error("coord number %d invalid",C[n]);
      return CG_ERROR;
    }
  }
  
  for (n = 0; n < zone->index_dim; n++) {
    dims[n] = zone->nijk[n] + zcoor->rind_planes[2*n] +
      zcoor->rind_planes[2*n+1];
    if (rmin[n] > rmax[n] || rmin[n] < 1 || rmax[n] > dims[n]) {
      cgi_error("Invalid index ranges.");
      return CG_ERROR;
    }
  }
  
  for (n = 0; n < 3; n++) {
    multi_info[n].mem_type_id = cgi_datatype(zcoor->coord[C[n]-1].data_type);
    multi_info[n].dset_id = (hid_t)zcoor->coord[C[n]-1].id;
  }
  
  multi_info[0].u.rbuf = coordsX;
  multi_info[1].u.rbuf = coordsY;
  multi_info[2].u.rbuf = coordsZ;
  
  return readwrite_multi_data_parallel(fn, multi_info,
					 zone->index_dim, rmin, rmax, 0);
}

int cgp_coords_write_data(int fn, int B, int Z, int *C,
    const cgsize_t *rmin, const cgsize_t *rmax, const void *coordsX, const void *coordsY, const void *coordsZ)
{
    int n;
    cgns_zone *zone;
    cgns_zcoor *zcoor;
    cgsize_t dims[3];
    cgsize_t index_dim;
    CGNS_ENUMT(DataType_t) type[3];
    H5D_rw_multi_t multi_info[3];

    cg = cgi_get_file(fn);
    if (check_parallel(cg)) return CG_ERROR;

    if (cgi_check_mode(cg->filename, cg->mode, CG_MODE_WRITE))
        return CG_ERROR;

    zone = cgi_get_zone(cg, B, Z);
    if (zone==0) return CG_ERROR;

    zcoor = cgi_get_zcoorGC(cg, B, Z);
    if (zcoor==0) return CG_ERROR;

    for (n = 0;  n < 3; n++) {
      if (C[n] > zcoor->ncoords || C[n] <= 0) {
        cgi_error("coord number %d invalid",C[n]);
        return CG_ERROR;
      }
    }

    for (n = 0; n < zone->index_dim; n++) {
        dims[n] = zone->nijk[n] + zcoor->rind_planes[2*n] +
                                  zcoor->rind_planes[2*n+1];
        if (rmin[n] > rmax[n] || rmin[n] < 1 || rmax[n] > dims[n]) {
            cgi_error("Invalid index ranges.");
            return CG_ERROR;
        }
    }
    
    for (n = 0; n < 3; n++) {
      multi_info[n].mem_type_id = cgi_datatype(zcoor->coord[C[n]-1].data_type);
      multi_info[n].dset_id = (hid_t)zcoor->coord[C[n]-1].id;
    }

    multi_info[0].u.wbuf = coordsX;
    multi_info[1].u.wbuf = coordsY;
    multi_info[2].u.wbuf = coordsZ;

    return readwrite_multi_data_parallel(fn, multi_info,
					 zone->index_dim, rmin, rmax, 1);
}

#endif
