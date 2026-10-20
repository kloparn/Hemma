# tools/image/debs.awk — which .deb files the image needs for `want` (tools/image/build.sh).
#   awk -v want="unattended-upgrades acl" -f debs.awk <the image's /var/lib/dpkg/status> <Debian's Packages>
# Prints "<package> <Filename> <SHA256> <Size>" for each package to bring - the wanted ones and every
# dependency the image doesn't have - and exits 1 when one can't be had: not in Debian's index, or
# needing an exact version of something the image has in another version.
function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
function bare(s) { s = trim(s); sub(/[ (].*$/, "", s); sub(/:any$/, "", s); return s }
function provides(list, where, pkg,   n, i, parts, name) {
    n = split(list, parts, ",")
    for (i = 1; i <= n; i++) {
        name = bare(parts[i])
        if (name == "") continue
        if (where == 1) instprov[name] = 1
        else if (!(name in availprov)) availprov[name] = pkg
    }
}
function flush() {
    if (pkg == "") return
    if (file == 1) {
        if (status ~ / installed$/) { inst[pkg] = ver; provides(prov, 1, pkg) }
    } else if (!(pkg in avail)) {
        avail[pkg] = ver; deps[pkg] = dep; fname[pkg] = fn; sum[pkg] = sha; size[pkg] = sz
        provides(prov, 2, pkg)
    }
    pkg = ver = status = prov = dep = fn = sha = sz = ""
}
FNR == 1 { flush(); file++ }
/^$/ { flush(); next }
/^Package: / { pkg = $2; next }
/^Version: / { ver = $2; next }
/^Status: / { status = $0; next }
/^Provides: / { prov = substr($0, 11); next }
/^(Pre-)?Depends: / { dep = (dep == "" ? "" : dep ", ") substr($0, index($0, ":") + 2); next }
/^Filename: / { fn = $2; next }
/^SHA256: / { sha = $2; next }
/^Size: / { sz = $2; next }
END {
    flush()
    nq = split(want, q, " ")
    for (head = 1; head <= nq; head++) {
        p = q[head]
        if (p in inst || p in add) continue
        if (!(p in avail)) { print "debs.awk: " p " isn't in Debian's index" > "/dev/stderr"; bad = 1; continue }
        add[p] = 1
        order[++na] = p
        ng = split(deps[p], groups, ",")
        for (g = 1; g <= ng; g++) {
            nalt = split(groups[g], alts, "|")
            ok = 0
            first = ""
            for (a = 1; a <= nalt && !ok; a++) {
                s = trim(alts[a])
                name = bare(s)
                cons = ""
                if (match(s, /\([^)]*\)/)) cons = trim(substr(s, RSTART + 1, RLENGTH - 2))
                if (name in inst || name in instprov || name in add) {
                    if (cons ~ /^= / && (name in inst) && inst[name] != trim(substr(cons, 2))) {
                        print "debs.awk: " p " needs " name " (" cons "), the image has " inst[name] > "/dev/stderr"
                        bad = 1
                    }
                    ok = 1
                } else if (first == "") {
                    if (name in avail) first = name
                    else if (name in availprov) first = availprov[name]
                }
            }
            if (ok) continue
            if (first == "") {
                print "debs.awk: nothing in Debian's index gives " trim(groups[g]) " (for " p ")" > "/dev/stderr"
                bad = 1
            } else q[++nq] = first
        }
    }
    for (i = 1; i <= na; i++) print order[i], fname[order[i]], sum[order[i]], size[order[i]]
    exit bad
}
