! File : runme.f90
program main
  use example
  implicit none
  type(Thing_Int) :: it
  type(Thing_Dbl) :: dt
  it = Thing_Int(123)
  call print_thing(it)
  dt = Thing_Dbl(456.d0)
  call print_thing(dt)

  call it%release()
  call dt%release()
end program


