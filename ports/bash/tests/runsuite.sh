# bash's own test suite, run on the machine: every NAME.tests by the
# shell, its output kept beside it. The comparison with NAME.right is
# done on the host (there is no diff here yet).
for n in ${BASH_TESTS:-$(for f in *.tests; do echo "${f%.tests}"; done)}; do
    f="$n.tests"
    bash "./$f" > "$n.out" 2>&1
    echo "ran $n"
done
echo SUITE-DONE
