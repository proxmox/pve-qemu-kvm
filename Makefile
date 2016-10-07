RELEASE=4.2

# also update debian/changelog
KVMVER=2.6.2
KVMPKGREL=2

KVMPACKAGE=pve-qemu-kvm
KVMDIR=qemu-kvm
KVMSRC=${KVMDIR}-src.tar.gz

ARCH=amd64
GITVERSION:=$(shell git rev-parse master)

DEBS=							\
${KVMPACKAGE}-dbg_${KVMVER}-${KVMPKGREL}_${ARCH}.deb	\
${KVMPACKAGE}_${KVMVER}-${KVMPKGREL}_${ARCH}.deb


all: ${DEBS}

.PHONY: download
download:
	rm -rf ${KVMDIR} ${KVMSRC}
	git clone --depth=1 git://git.qemu-project.org/qemu.git -b v${KVMVER} ${KVMDIR}
	tar czf ${KVMSRC} --exclude CVS --exclude .git --exclude .svn ${KVMDIR}

.PHONY: deb
deb ${DEBS} kvm: ${KVMSRC}
	rm -f *.deb
	rm -rf ${KVMDIR}
	tar xf ${KVMSRC} 
	cp -a debian ${KVMDIR}/debian
	echo "git clone git://git.proxmox.com/git/pve-qemu-kvm.git\\ngit checkout ${GITVERSION}" > ${KVMDIR}/debian/SOURCE
	# set package version
	sed -i 's/^pkgversion="".*/pkgversion="${KVMPACKAGE}_${KVMVER}-${KVMPKGREL}"/' ${KVMDIR}/configure
	cd ${KVMDIR}; dpkg-buildpackage -b -rfakeroot -us -uc
	lintian ${DEBS} || true

.PHONY: upload
upload: ${DEBS} ${KVMDIR}-src.tar.gz
	tar cf - ${DEBS} | ssh repoman@repo.proxmox.com upload

.PHONY: distclean
distclean: clean


.PHONY: clean
clean:
	rm -rf *~ ${KVMDIR} ${KVMPACKAGE}_* ${DEBS}

.PHONY: dinstall
dinstall: ${DEBS}
	dpkg -i ${DEBS}
