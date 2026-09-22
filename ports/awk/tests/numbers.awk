# Arithmetic and formatting: the 68040's FPU and picolibc's printf.
BEGIN {
    printf "%d %.3f %e %x %o %5.1f|%-6s|%c\n", 7/2, 1/3, 12345.678, 255, 8, 3.14159, "ab", 65
    print 2^31, 2^53, 2^-2, int(-3.7), int(3.7), -7 % 3, 7 % -3
    printf "%.6f %.6f %.6f %.6f %.6f\n", sqrt(2), exp(1), log(10), sin(1), atan2(1, 1) * 4
    print (0.1 + 0.2 == 0.3), 1e300 * 10, -1e300 * 10, 1 / 3
    print "10" < "9", 10 < 9, "abc" < "abd", 1e3, 0x10 + 0, "3x" + 4
    x = 17; x += 3; x *= 2; x -= 1; x /= 3; x ^= 2; print x
    print length(12345), 1000000, 123456789012, 0.000001
}
