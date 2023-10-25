FROM  devkitpro/devkita64:20230929  as  base

# install dependencies
RUN   apt-get  update       \
  &&  apt-get  install  -y  \
    automake                \
    build-essential         \
    fakeroot                \
    file                    \
    zstd                    \
;

# install devkitpro
RUN   useradd  nxdt-build  \
  &&  dkp-pacman  --noconfirm  -S  dkp-toolchain-vars  \
;

WORKDIR  /app/

################################################################################

FROM  base  as  lwext4

COPY  ./libs/libusbhsfs/liblwext4  ./libs/libusbhsfs/liblwext4
RUN  chown  nxdt-build:  -R  .
USER  nxdt-build

RUN   cd ./libs/libusbhsfs/liblwext4  \
  &&  dkp-makepkg  -c  -C  -f  \
  &&  cd  /app/  \
  &&  cp  ./libs/libusbhsfs/liblwext4/switch-lwext4*.tar.zst  lwext4.tar.zst  \
;

################################################################################

FROM  base  as  ntfs3g

COPY  ./libs/libusbhsfs/libntfs-3g  ./libs/libusbhsfs/libntfs-3g
RUN  chown  nxdt-build:  -R  .
USER  nxdt-build

RUN   cd ./libs/libusbhsfs/libntfs-3g  \
  &&  dkp-makepkg  -c  -C  -f  \
  &&  cd  /app/  \
  &&  cp  ./libs/libusbhsfs/libntfs-3g/switch-libntfs-3g*.tar.zst  ntfs3g.tar.zst  \
;

################################################################################

FROM  base  as  builder

COPY  --from=lwext4  /app/lwext4.tar.zst  ./lwext4.tar.zst
COPY  --from=ntfs3g  /app/ntfs3g.tar.zst  ./ntfs3g.tar.zst
RUN   dkp-pacman  -U lwext4.tar.zst  --needed  --noconfirm  \
  &&  dkp-pacman  -U ntfs3g.tar.zst  --needed  --noconfirm  \
  &&  rm  -rf  lwext4.tar.zst  ntfs3g.tar.zst  \
;

ENTRYPOINT  [ "/app/build-downgrade.sh" ]
