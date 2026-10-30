# syntax=docker/dockerfile:1.7
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
ARG SOURCE_DATE_EPOCH
ARG SANITIZER_FLAGS
ARG COVERAGE_FLAGS=
ARG MAYHEM_JOBS=

USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Build dependencies. Debian's libvulkan-dev is deliberately NOT installed: it is older than the pinned
# Vulkan-Headers and meson would prefer it over the subproject.
USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
        meson cython3 python3-dev python3-setuptools libzimg-dev \
 && rm -rf /var/lib/apt/lists/*

# meson's glslang / vulkan-headers wraps are git subprojects; fetch the exact revisions the wraps pin
# (plus upstream's packagefiles overlay) into the image so build.sh can re-run offline.
RUN set -eu; \
    for dep in glslang vulkan-headers; do \
        url=$(sed -n 's/^url *= *//p' "/mayhem/subprojects/$dep.wrap"); \
        rev=$(sed -n 's/^revision *= *//p' "/mayhem/subprojects/$dep.wrap"); \
        git clone --depth 1 --branch "$rev" "$url" "/opt/toolchains/$dep"; \
        cp -r "/mayhem/subprojects/packagefiles/$dep/." "/opt/toolchains/$dep/"; \
        chmod -R a+rX "/opt/toolchains/$dep"; \
    done

USER mayhem
WORKDIR /mayhem

RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
