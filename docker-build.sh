#!/bin/bash

set -euo pipefail

VERS=""
if [ $# -eq 1 ] && [[ -n "$1" ]] ; then
  VERS="-e APP_VERSION=$1"
fi

export DOCKER_BUILDKIT=1

docker  buildx  build  --file ./Dockerfile  --tag smo-downgrade-build  --load  .

docker  run  --rm        \
  -u $(id -u):$(id -g)   \
  -v "/$PWD/":/app/      \
  $VERS                  \
  smo-downgrade-build    \
;

docker  rmi  smo-downgrade-build
