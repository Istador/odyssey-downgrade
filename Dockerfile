FROM  devkitpro/devkita64:latest  as  base

# install dependencies
RUN   apt-get  update       \
  &&  apt-get  install  -y  \
    automake                \
    build-essential         \
    fakeroot                \
    file                    \
;

# install devkitpro
RUN   ln  -s  /proc/self/mounts  /etc/mtab  \
  &&  useradd  nxdt-build  \
  &&  dkp-pacman  --noconfirm  -S  dkp-toolchain-vars  \
;

WORKDIR  /app/

################################################################################

FROM  base  as  lwext4

COPY  ./libs/libusbhsfs/liblwext4  ./libs/libusbhsfs/liblwext4
RUN  chown  nxdt-build:  -R  .
USER  nxdt-build

RUN   cd ./libs/libusbhsfs/liblwext4  \
  &&  COMMIT=docker  dkp-makepkg  \
  &&  cd  /app/  \
  &&  cp  ./libs/libusbhsfs/liblwext4/switch-lwext4*.tar.xz  lwext4.tar.xz  \
;

################################################################################

FROM  base  as  ntfs3g

COPY  ./libs/libusbhsfs/libntfs-3g  ./libs/libusbhsfs/libntfs-3g
RUN  chown  nxdt-build:  -R  .
USER  nxdt-build

RUN   cd ./libs/libusbhsfs/libntfs-3g  \
  &&  COMMIT=docker  dkp-makepkg  \
  &&  cd  /app/  \
  &&  cp  ./libs/libusbhsfs/libntfs-3g/switch-libntfs-3g*.tar.xz  ntfs3g.tar.xz  \
;

################################################################################

FROM  base  as  builder

COPY  --from=lwext4  /app/lwext4.tar.xz  ./lwext4.tar.xz
COPY  --from=ntfs3g  /app/ntfs3g.tar.xz  ./ntfs3g.tar.xz
RUN   dkp-pacman  -U lwext4.tar.xz  --noconfirm  \
  &&  dkp-pacman  -U ntfs3g.tar.xz  --noconfirm  \
  &&  rm  -rf  lwext4.tar.xz  ntfs3g.tar.xz  \
;

ENTRYPOINT  [ "/app/build-downgrade.sh" ]
