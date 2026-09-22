# The shell language, run by bash on the machine and by bash on the
# host, and compared. Nothing here may depend on where it runs: no
# pids, times, paths or hostnames.
echo "== expansion"
name=world; echo "hello $name ${name}s ${#name} ${name^^} ${name/o/0}"
path=/a/b/c.txt; echo "${path##*/} ${path%/*} ${path%.*} ${path//\//|}"
unset u; echo "[${u:-default}] [${u:+set}] [${u-x}]"
set -- one two three; echo "$# $1 $3 $* ${@: -1}"
echo "{1..5} $(echo {1..5}) {a..e} $(echo {a..e}) $(echo x{1,2}y)"
printf '%s|%d|%05.2f|%x|%c|%b\n' str 42 3.14159 255 A 'a\tb'
echo "== arithmetic"
i=7; echo $((i * 6)) $((i % 4)) $((2 ** 10)) $((i > 3 ? 10 : 20)) $((0x1f)) $((8#17))
((i += 3)); echo "i=$i"
for ((j = 0; j < 3; j++)); do printf 'j%d ' "$j"; done; echo
echo "== test and [["
[[ abc == a* ]] && echo glob-match
[[ abc =~ ^a(b)c$ ]] && echo "regex-match ${BASH_REMATCH[1]}"
[[ 5 -gt 3 && -n str ]] && echo and-ok
[ -d / ] && [ ! -f /nosuchthing ] && echo file-tests-ok
case xyz in a*) echo no ;; x*) echo case-ok ;; *) echo no ;; esac
echo "== arrays"
arr=(alpha beta gamma); arr[5]=zeta
echo "${arr[0]} ${arr[2]} ${#arr[@]} ${arr[@]: -1} ${!arr[@]}"
declare -A m; m[one]=1; m[two]=2
for k in one two; do printf '%s=%s ' "$k" "${m[$k]}"; done; echo
echo "== functions, locals, recursion"
fact() { local n=$1; if [ "$n" -le 1 ]; then echo 1; else echo $((n * $(fact $((n - 1))))); fi; }
echo "10! = $(fact 10)"
counter=0; bump() { counter=$((counter + 1)); }; bump; bump; echo "counter=$counter"
echo "== loops and control"
s=; for w in a b c; do s="$s$w"; done; echo "for: $s"
n=0; while [ $n -lt 3 ]; do n=$((n + 1)); done; echo "while: $n"
n=5; until [ $n -le 2 ]; do n=$((n - 1)); done; echo "until: $n"
for w in a b c d; do [ "$w" = b ] && continue; [ "$w" = d ] && break; printf '%s ' "$w"; done; echo
echo "== pipes, substitution, redirection"
echo "one two three" | tr ' ' '\n' | sort -r | tr '\n' ' '; echo
echo "cmdsub: $(echo nested $(echo deeper))"
echo "backtick: `echo old-style`"
cat > tmp.$$ <<EOF
here-doc $name
$((6 * 7))
EOF
cat tmp.$$; rm -f tmp.$$
cat <<'EOF'
unexpanded $name
EOF
echo a-line | { read -r first; echo "read: [$first]"; }
echo "== exit status and lists"
true && echo t-ok; false || echo f-ok
(exit 3); echo "status=$?"
! false; echo "negated=$?"
echo "== jobs"
sleep 1 & pid=$!; wait $pid; echo "waited=$?"
echo "== traps and subshells"
( trap 'echo trapped-exit' EXIT; echo in-subshell )
x=outer; ( x=inner; echo "sub sees $x" ); echo "parent still $x"
echo "== getopts"
set -- -a -b value arg
while getopts "ab:" opt; do case $opt in a) echo "opt a" ;; b) echo "opt b=$OPTARG" ;; esac; done
shift $((OPTIND - 1)); echo "left: $*"
echo "== string builtins"
printf 'padded:[%10s][%-10s]\n' right left
echo "upper: ${name^^} lower: ${name,,} len: ${#path}"
IFS=: read -r a b c <<< "x:y:z"; echo "ifs-read: $a-$b-$c"
echo "== done"
