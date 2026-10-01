# bash's own test suite, run on the machine, as its run-all does: each
# test through its own run-NAME script, which is where a test's output
# is filtered before it is compared -- run-varenv drops the lines its
# test prints to say what it EXPECTS, for one, and running varenv.tests
# directly kept them and failed. A test with no run-NAME is run as
# itself.
#
# The scripts end `diff $BASH_TSTOUT NAME.right && rm -f $BASH_TSTOUT`.
# The comparison is done on the host, so `diff` here is a stand-in that
# answers "different", and the output stays for the host to read.
mkdir -p shim
printf '#!/BT/tests/bash\nexit 1\n' > shim/diff
chmod 755 shim/diff
for n in ${BASH_TESTS:-$(for f in *.tests; do echo "${f%.tests}"; done)}; do
    if [ -f "run-$n" ]; then
        BASH_TSTOUT="$PWD/$n.out" PATH="$PWD/shim:$PATH" bash "./run-$n" \
            > "$n.run" 2>&1
    else
        bash "./$n.tests" > "$n.out" 2>&1
    fi
    echo "ran $n"
done
echo SUITE-DONE
