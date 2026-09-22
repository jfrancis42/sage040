# The forms of getline, and NR/FNR/FILENAME over two inputs.
NR == 1 { getline; print "after plain getline:", $0, NR }
NR == 3 { getline v; print "getline var:", v, "$0 still", $0, NR }
FNR == 1 && NR > 1 { print "new file", FILENAME, FNR, NR }
END { print "total", NR }
