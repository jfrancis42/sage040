# Output to a file, appending, closing, and reading it back.
BEGIN {
    f = "files.tmp"
    print "first" > f
    print "second" > f
    close(f)
    print "third" >> f
    close(f)
    while ((getline line < f) > 0) print "read:", line
    close(f)
    print (getline line < "no-such-file")
    printf "%s\n", "to stderr" > "/dev/stderr"
    system("rm files.tmp")
}
