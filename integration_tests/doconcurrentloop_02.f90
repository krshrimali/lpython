program doconcurrentloop_02
implicit none

real, dimension(10) :: a
real :: sum
integer :: N, i

N = size(a)
sum = 0

do concurrent (i = 1:N)
    a(i) = a(i-1) + 5
end do

call arraySum(a, sum)

contains
    subroutine arraySum(a, sum)
        real, intent(in) :: a(:)
        real, intent(out) :: sum
        do concurrent (i = 1:N) reduce(+: s)
            sum = sum + a(i)
            print *, sum
        end do
    end subroutine
end program
