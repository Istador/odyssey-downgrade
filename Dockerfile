FROM  ubuntu:20.04  as  base

ENV  DEBIAN_FRONTEND  noninteractive

# install dependencies
RUN   apt-get  update       \
  &&  apt-get  install  -y  \
    apt-transport-https     \
    automake                \
    build-essential         \
    cmake                   \
    curl                    \
    fakeroot                \
    file                    \
    git                     \
    p7zip-full              \
;

# install devkitpro
RUN   ln  -s  /proc/self/mounts  /etc/mtab  \
  &&  useradd  nxdt-build  \
  &&  mkdir  /devkitpro/  \
  &&  echo "deb [signed-by=/devkitpro/pub.gpg] https://apt.devkitpro.org stable main" >/etc/apt/sources.list.d/devkitpro.list  \
  &&  curl --fail  -o /devkitpro/pub.gpg  https://apt.devkitpro.org/devkitpro-pub.gpg  \
  &&  apt-get update  \
  &&  apt-get  install  -y  devkitpro-pacman  \
  &&  dkp-pacman  --noconfirm  -S  \
    dkp-toolchain-vars  \
    switch-dev  \
;

WORKDIR  /app/

ENV  DEVKITPRO  /opt/devkitpro
ENV  DEVKITARM  /opt/devkitpro/devkitARM
ENV  DEVKITPPC  /opt/devkitpro/devkitPPC

################################################################################

FROM  base  as  lwext4

COPY  ./libs/libusbhsfs/liblwext4  ./libs/libusbhsfs/liblwext4
RUN  chown  nxdt-build:  -R  .
USER  nxdt-build

RUN   cd ./libs/libusbhsfs/liblwext4  \
  &&  COMMIT=docker  dkp-makepkg  \
  &&  cd /app/  \
  &&  cp  ./libs/libusbhsfs/liblwext4/switch-lwext4*.tar.xz  lwext4.tar.xz  \
;

################################################################################

FROM  base  as  ntfs3g

COPY  ./libs/libusbhsfs/libntfs-3g  ./libs/libusbhsfs/libntfs-3g
RUN  chown  nxdt-build:  -R  .
USER  nxdt-build

RUN   cd ./libs/libusbhsfs/libntfs-3g  \
  &&  COMMIT=docker  dkp-makepkg  \
  &&  cd /app/  \
  &&  cp  ./libs/libusbhsfs/libntfs-3g/switch-libntfs-3g*.tar.xz  ntfs3g.tar.xz  \
;

################################################################################

FROM  base  as  builder

RUN  dkp-pacman  --noconfirm  -S  \
  switch-glfw  \
  switch-libjson-c  \
  switch-curl  \
  switch-glad  \
  switch-glm  \
  switch-mbedtls  \
  switch-libxml2  \
;

COPY  --from=lwext4  /app/lwext4.tar.xz  ./lwext4.tar.xz
COPY  --from=ntfs3g  /app/ntfs3g.tar.xz  ./ntfs3g.tar.xz
RUN   dkp-pacman  -U lwext4.tar.xz  --noconfirm  \
  &&  dkp-pacman  -U ntfs3g.tar.xz  --noconfirm  \
  &&  rm  -rf  lwext4.tar.xz  ntfs3g.tar.xz  \
;

ENTRYPOINT  [ "/app/build-downgrade.sh" ]
