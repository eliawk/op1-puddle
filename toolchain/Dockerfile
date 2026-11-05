FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends build-essential wget ca-certificates xz-utils \
    libgmp-dev libmpfr-dev libmpc-dev texinfo bison flex python3 file && rm -rf /var/lib/apt/lists/*
WORKDIR /build
ARG BU=2.42
ARG GCC=13.3.0
RUN wget -q https://sourceware.org/pub/binutils/releases/binutils-${BU}.tar.xz && tar xf binutils-${BU}.tar.xz && \
    mkdir bu && cd bu && ../binutils-${BU}/configure --target=bfin-elf --prefix=/opt/bfin --disable-nls --disable-werror \
    --disable-gdb --disable-sim && make -j$(nproc) && make install && cd .. && rm -rf bu binutils-${BU}*
ENV PATH=/opt/bfin/bin:$PATH
RUN wget -q https://gcc.gnu.org/pub/gcc/releases/gcc-${GCC}/gcc-${GCC}.tar.xz && tar xf gcc-${GCC}.tar.xz && \
    mkdir g && cd g && ../gcc-${GCC}/configure --target=bfin-elf --prefix=/opt/bfin --disable-nls --enable-languages=c \
    --without-headers --with-newlib --disable-libssp --disable-libquadmath --disable-shared --disable-threads && \
    make -j$(nproc) all-gcc all-target-libgcc && make install-gcc install-target-libgcc && cd .. && rm -rf g gcc-${GCC}*
