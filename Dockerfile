FROM  devkitpro/devkita64:20260219  AS  builder

# install dependencies
RUN   apt-get  update       \
  &&  apt-get  install  -y  \
    automake                \
    build-essential         \
    fakeroot                \
    file                    \
    zstd                    \
;

# install & update devkitpro
RUN   useradd  nxdt-build  \
  &&  dkp-pacman  --noconfirm  -S  dkp-toolchain-vars  \
  &&  dkp-pacman  --noconfirm  -Sy  \
  &&  dkp-pacman  --noconfirm  -Syu  \
;

# build a newer libnx version
RUN  git  clone  --branch master  --single-branch  https://github.com/switchbrew/libnx.git  \
  &&  cd  libnx               \
  &&  git  checkout  b156b47  \
  &&  make  -j$(nproc)        \
  &&  make  install           \
  &&  cd  ..                  \
  &&  rm  -rf  libnx          \
;

WORKDIR  /app/

ENTRYPOINT  [ "/app/build-downgrade.sh" ]
