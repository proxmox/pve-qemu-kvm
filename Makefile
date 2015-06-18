RELEASE=4.0

# also update debian/changelog
KVMVER=2.3
KVMPKGREL=4

KVMPACKAGE=pve-qemu-kvm
KVMDIR=qemu-kvm
KVMSRC=${KVMDIR}-src.tar.gz

ARCH=amd64
GITVERSION:=$(shell cat .git/refs/heads/master)

DEBS=							\
${KVMPACKAGE}-dbg_${KVMVER}-${KVMPKGREL}_${ARCH}.deb	\
${KVMPACKAGE}_${KVMVER}-${KVMPKGREL}_${ARCH}.deb


all: ${DEBS}

.PHONY: download
download:
	rm -rf ${KVMDIR} ${KVMSRC}
	#git clone git://git.qemu-project.org/qemu.git -b stable-2.2 ${KVMDIR} 
	git clone git://git.qemu-project.org/qemu.git ${KVMDIR} 
	cd ${KVMDIR}; git checkout v2.3.0
	tar czf ${KVMSRC} --exclude CVS --exclude .git --exclude .svn ${KVMDIR}

${DEBS} kvm: ${KVMSRC}
	rm -rf ${KVMDIR}
	tar xf ${KVMSRC} 
	cp -a debian ${KVMDIR}/debian
	echo "git clone git://git.proxmox.com/git/pve-qemu-kvm.git\\ngit checkout ${GITVERSION}" > ${KVMDIR}/debian/SOURCE
	cd ${KVMDIR}; dpkg-buildpackage -b -rfakeroot -us -uc
	lintian ${DEBS} || true

.PHONY: upload
upload: ${DEBS} ${KVMDIR}-src.tar.gz
	umount /pve/${RELEASE}; mount /pve/${RELEASE} -o rw 
	mkdir -p /pve/${RELEASE}/extra
	rm -rf /pve/${RELEASE}/extra/Packages*
	rm -rf /pve/${RELEASE}/extra/${KVMPACKAGE}_*.deb
	rm -rf /pve/${RELEASE}/extra/${KVMPACKAGE}-dbg_*.deb
	cp ${DEBS} /pve/${RELEASE}/extra
	cd /pve/${RELEASE}/extra; dpkg-scanpackages . /dev/null | gzip -9c > Packages.gz
	umount /pve/${RELEASE}; mount /pve/${RELEASE} -o ro

.PHONY: distclean
distclean: clean


.PHONY: clean
clean:
	rm -rf *~ ${KVMDIR} ${KVMPACKAGE}_* ${DEBS}

.PHONY: dinstall
dinstall: ${DEBS}
	dpkg -i ${DEBS}
