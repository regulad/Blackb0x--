# Point curl at the CA store cainjector installs.
#
# A DROP-IN, NOT /etc/profile ITSELF. That file is owned by the `profile.d`
# package (already installed -- it is line 48 of package/packages.txt, and
# coreutils Depends: on it), whose /etc/profile is exactly the loader this
# relies on:
#
#     for i in /etc/profile.d/*.sh ; do
#         if [ -r "$i" ]; then . $i; fi
#     done
#
# so the .sh suffix and being readable are the whole contract. coreutils and
# less already ship drop-ins the same way. An earlier version of this package
# shipped /etc/profile directly, which would have collided with profile.d over
# a file it owns.
#
# WHY CURL NEEDS TELLING AT ALL. cainjector installs the Debian CA store into
# Apple's admin trust store (via SecTrustStoreSetTrustSettings, which is what
# apt's CFNetwork transport consults) and as a plain PEM at
# /usr/lib/ssl/cert.pem (OpenSSL's own compiled-in default -- OPENSSLDIR is
# "/usr/lib/ssl" on this build). Neither reaches curl: its libcurl imports
# _SSL_CTX_load_verify_locations but NOT _SSL_CTX_set_default_verify_paths,
# and `curl-config --ca` is empty, so it was built with no bundle of its own
# and never falls back to OpenSSL's defaults.
#
# SHELLS ONLY. There is no global environment mechanism on this device to do
# better -- /etc/environment is a pam_env convention and no pam_env.so ships
# in either the pam or pam-modules package, and this firmware's launchd has no
# reference to /etc/launchd.conf at all. A daemon gets nothing from this file;
# see the postinstall LaunchDaemon's own EnvironmentVariables for that case.
export CURL_CA_BUNDLE=/usr/lib/ssl/cert.pem
