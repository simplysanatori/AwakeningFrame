# Sourced by steam-frame/build.sh (crash reports, runtime/crash_report.c): the Sentry DSN that Send Report sends to.
# From AF_SENTRY_DSN, else the one line of AF_SENTRY_DSN_FILE (default ~/.config/awakeningframe-dev/sentry-dsn) on
# this machine. Never commit it: without one the release has no Send Report and nobody is asked. Says whether one was
# found, never the DSN itself.
if [ -z "${AF_SENTRY_DSN:-}" ]; then
    _dsn_file=${AF_SENTRY_DSN_FILE:-$HOME/.config/awakeningframe-dev/sentry-dsn}
    if [ -r "$_dsn_file" ]; then
        AF_SENTRY_DSN=$(head -n 1 "$_dsn_file" | tr -d '[:space:]')
    fi
    unset _dsn_file
fi
export AF_SENTRY_DSN=${AF_SENTRY_DSN:-}
if [ -n "$AF_SENTRY_DSN" ]; then
    echo "crash reports: Send Report goes to the Sentry DSN given (AF_SENTRY_DSN)"
else
    echo "crash reports: no Sentry DSN (AF_SENTRY_DSN or ~/.config/awakeningframe-dev/sentry-dsn): no Send Report"
fi
