	program write_axisym
	USE CGNS
	implicit none

!       author: Diane Poirier (diane@icemcfd.com)
!       last revised on June 7 2002

!       This example tests the Axisymmetry_t data structure and its children

#ifdef WINNT
	include 'cgnswin_f.h'
#endif

	integer Ndim
	parameter (Ndim = 2)
	integer index_dim, cell_dim, phys_dim, i
	integer cg, base_no, ier
        integer(cgsize_t) dimval(2)
	real*4 ref_point(Ndim), axis(Ndim), angle
	character*100 linkpath, DescriptorText
	character*32 CoordinateNames(2), DescriptorName


! *** initialize
	ier = 0
	index_dim=Ndim
	cell_dim=Ndim
	phys_dim=Ndim

! *** open CGNS file for writing
 	call cg_open_f('Test_V2', MODE_WRITE, cg, ier)
 	if (ier .eq. ERROR) call cg_error_exit_f

! *** base
 	call cg_base_write_f(cg, 'Basename', cell_dim, phys_dim,
     &                       base_no, ier)
 	if (ier .eq. ERROR) call cg_error_exit_f

! *** write a descriptor under the base
        call cg_goto_f(cg, base_no, ier, 'end')
	if (ier .eq. ERROR) call cg_error_exit_f

	call cg_descriptor_write_f('BaseDescriptorName',
     &      'BaseDescriptorText', ier)
	if (ier .eq. ERROR) call cg_error_exit_f

! *** write Axisymmetry_t node
	do i=1,Ndim
	    ref_point(i)=0
	    axis(i) = i-1
	enddo

	call cg_axisym_write_f(cg, base_no, ref_point, axis, ier)
	if (ier .eq. ERROR) call cg_error_exit_f

! *** write children of Axisymmetry_t
!234567890!234567890!234567890!234567890!234567890!234567890!23456789012
	call cg_goto_f(cg, base_no, ier, 'Axisymmetry_t', 1, 'end')
	if (ier .eq. ERROR) call cg_error_exit_f

! * DataArray_t AxisymmetryAngle
	angle = 360
	dimval(1)=1
	call cg_array_write_f('AxisymmetryAngle', RealSingle, 1,
     &       1_cgsize_t, angle, ier)
	if (ier .eq. ERROR) call cg_error_exit_f

! * DataArray_t CoordinateNames
        dimval(1)=32
        dimval(2)=2
	write(CoordinateNames(1),'(a)')'CoordinateX'
	write(CoordinateNames(2),'(a)')'CoordinateZ'
	call cg_array_write_f('CoordinateNames', Character, 2,
     &       dimval, CoordinateNames, ier)
	if (ier .eq. ERROR) call cg_error_exit_f

! * Descriptor
	call cg_descriptor_write_f('AxisymDescriptorName',
     &       'AxisymDescriptorText', ier)
        if (ier .eq. ERROR) call cg_error_exit_f

! * DataClass
        call cg_dataclass_write_f(Dimensional, ier)
	if (ier .eq. ERROR) call cg_error_exit_f

! * DimensionalUnits
	call cg_units_write_f(Kilogram, Meter, Second, Kelvin,
     &                        Radian, ier)
	if (ier .eq. ERROR) call cg_error_exit_f

! * UserDefinedData
        call cg_user_data_write_f('UserData1', ier)
        if (ier .eq. ERROR) call cg_error_exit_f

! * Link to BaseDescriptorName
        write(linkpath,'(a)') 'Basename/BaseDescriptorName'
        call cg_link_write_f('LinkToBaseDescr', ' ',linkpath, ier)
        if (ier .eq. ERROR) call cg_error_exit_f

! * DataArray_t/Descriptor
	do i=1, 4
!234567890!234567890!234567890!234567890!234567890!234567890!23456789012
	    call cg_goto_f(cg, base_no, ier, 'Axisymmetry_t', 1,
     &                     'DataArray_t', i, 'end')
            if (ier .eq. ERROR) call cg_error_exit_f
	    write(DescriptorName,'(a,i1)') 'DescriptorName4Array',i
	    write(DescriptorText,'(a,i1)') 'DescriptorText4Array',i
	    call cg_descriptor_write_f(DescriptorName,
     &           DescriptorText, ier)
	    if (ier .eq. ERROR) call cg_error_exit_f

! * DataArray_t/DimensionalUnits
	    call cg_units_write_f(Kilogram, Meter, Second, Kelvin,
     &                            Degree, ier)
            if (ier .eq. ERROR) call cg_error_exit_f
	enddo

! *** close CGNS file
	call cg_close_f(cg, ier)
	if (ier .eq. ERROR) call cg_error_exit_f
	end
