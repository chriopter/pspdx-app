# The parts dev/localcat/run and dev/start --mock both need: one work
# directory, one throwaway CA that the client is built to trust and nothing
# else, and the two builds that switch the client between the loopback
# catalog and the published one.
#
# Sourced, not run. The caller sets APP to the absolute path of app/ first:
#
#   APP="$HERE/app"; . "$HERE/dev/localcat/common.sh"
: "${APP:?dev/localcat/common.sh needs APP set to app/}"
REPO="$(cd "$APP/.." && pwd)"
LOCALCAT="$REPO/dev/localcat"
WORK="${LOCALCAT_WORK:-$LOCALCAT/work}"
LOCALCAT_PORT=8443
LOCALCAT_URL="https://127.0.0.1:$LOCALCAT_PORT/catalog.json"
# Set while the client's objects are built against the loopback URL, cleared
# when they are built against the published catalog again, so a later run can
# tell which of the two the EBOOT on the stick is.
LOCALCAT_MARK="$WORK/built-local"

# A CA, a certificate for the loopback address, and the header that makes the
# client trust that CA alone. Once; the files last thirty days.
localcat_certs() {
	mkdir -p "$WORK"
	[ -f "$WORK/ca.crt" ] && return 0
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
		-keyout "$WORK/ca.key" -out "$WORK/ca.crt" -days 30 -subj "/CN=localcat CA" >/dev/null 2>&1
	openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
		-keyout "$WORK/srv.key" -out "$WORK/srv.csr" -subj "/CN=127.0.0.1" >/dev/null 2>&1
	printf 'subjectAltName=IP:127.0.0.1\n' > "$WORK/san.cnf"
	openssl x509 -req -in "$WORK/srv.csr" -CA "$WORK/ca.crt" -CAkey "$WORK/ca.key" \
		-CAcreateserial -out "$WORK/srv.crt" -days 30 -extfile "$WORK/san.cnf" >/dev/null 2>&1
	{
		echo '/* Throwaway CA for dev/localcat; defines ca_certs.h'"'"'s guard so'
		echo '   the client trusts this and nothing else. Generated. */'
		echo '#ifndef PSPDX_CA_CERTS_H'
		echo '#define PSPDX_CA_CERTS_H'
		echo 'static const char PSPDX_CA_PEM[] ='
		sed 's/.*/"&\\n"/' "$WORK/ca.crt"
		echo ';'
		echo '#endif'
	} > "$WORK/testca.h"
}

# The two objects that see the catalog URL or the CA, and only those: the
# rest of the build is the same either way and is left alone. What decides
# is make's own exit status, not what the log says: a build that failed
# leaves the previous EBOOT.PBP lying there, and that one is built for the
# other catalog.
#
# The whole repository is mounted, not app/ alone as the real build mounts
# it: the test build names testca.h relative to app/, and the header lives
# under dev/, which an app/-only mount would not carry into the container.
localcat_rebuild() {
	mkdir -p "$WORK"
	# Every object the flags below reach: the TLS stack for the CA, the
	# catalog for the URL, and sources for the test fixtures. One left out
	# keeps the build it was made for, and a mock build then shows no mock.
	if (cd "$APP" && rm -f network/https.o update/catalog.o update/sources.o && \
		docker run --rm -v "$REPO:/src" -w /src/app pspdev/pspdev:latest make "$@") \
		>"$WORK/build.log" 2>&1
	then
		grep -E 'error|EBOOT\.PBP' "$WORK/build.log" || true
		[ -f "$APP/EBOOT.PBP" ]
	else
		echo "build failed; the end of $WORK/build.log:" >&2
		tail -n 20 "$WORK/build.log" >&2
		return 1
	fi
}

# Against the host: the throwaway CA folded in and the catalog URL replaced.
localcat_build_local() {
	localcat_certs
	# make runs inside the container with app/ as the working directory and
	# the repository mounted, so the header is named relative to app/ --
	# ../dev/localcat/work/testca.h where the work directory lies by default
	# -- and therefore has to be somewhere under the repository.
	case "$WORK" in
		"$REPO"/*) rel="../${WORK#"$REPO"/}" ;;
		*) echo "LOCALCAT_WORK must be under $REPO for the build to see testca.h" >&2; return 1 ;;
	esac
	localcat_rebuild EXTRA_CFLAGS="-DPSPDX_TEST_FIXTURES -include $rel/testca.h -DCATALOG_URL='\"$LOCALCAT_URL\"'" \
		|| return 1
	: > "$LOCALCAT_MARK"
}

# Back to the published catalog and the real CA bundle.
localcat_build_real() {
	localcat_rebuild || return 1
	rm -f "$LOCALCAT_MARK"
}

# True while the EBOOT that was last built points at the host.
localcat_is_local() { [ -f "$LOCALCAT_MARK" ]; }

# The films out of a catalog site built next door, planted on the stick
# under the name an entry that links no film is looked up by, so a row has
# something to play before anything is fetched. The catalog keeps one
# directory an app, and the directory is the id. The desk and the rig plant
# the same ones the same way.
localcat_plant_films() {
	mkdir -p "$1/PSP/PSPDX/CACHE/media"
	for clip in "$REPO"/../pspdx-catalog/site/apps/*/film-*.pmf \
	            "$REPO"/catalog/site/apps/*/film-*.pmf; do
		[ -f "$clip" ] || continue
		cp "$clip" "$1/PSP/PSPDX/CACHE/media/$(basename "$(dirname "$clip")").mp4"
	done
}

# dev/start --mock leaves a server behind on purpose, as a user unit, so it
# outlives the shell that started it. Anything else that wants the port --
# the rig, or a run without --mock -- takes it back here first.
localcat_stop_server() {
	systemctl --user stop pspdx-mock-server 2>/dev/null || true
}

# The same directory over TLS, as a unit that outlives this shell.
localcat_start_server() {
	localcat_stop_server
	systemd-run --user --collect --quiet --unit=pspdx-mock-server \
		python3 "$LOCALCAT/serve.py" "$1" "$WORK/srv.crt" "$WORK/srv.key" "$LOCALCAT_PORT"
}
