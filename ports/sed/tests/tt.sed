# t and T: branch on substitution; q with an exit code.
s/^k1/K1/
t done
s/$/ (not k1)/
T
:done
s/$/ !/
/v3/q5
