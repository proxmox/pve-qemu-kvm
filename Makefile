RELEASE=2.0

# also update debian/changelog
KVMVER=1.0
KVMPKGREL=1

KVMPACKAGE=pve-qemu-kvm
KVMDIR=qemu-kvm

ARCH=amd64

KVM_DEB=${KVMPACKAGE}_${KVMVER}-${KVMPKGREL}_${ARCH}.deb

all: ${KVM_DEB} ${KVMDIR}-src.tar.gz

${KVMDIR}.org/README:
	rm -rf ${KVMDIR}.org
	git clone git://git.kernel.org/pub/scm/virt/kvm/qemu-kvm.git ${KVMDIR}.org
	cd ${KVMDIR}.org; git checkout -b local qemu-kvm-${KVMVER}
	touch $@

${KVMDIR}-src.tar.gz: ${KVMDIR}.org/README
	tar czf $@ ${KVMDIR}.org

${KVM_DEB} kvm: ${KVMDIR}.org/README
	rm -rf ${KVMDIR}
	cp -a ${KVMDIR}.org ${KVMDIR}
	cp -a debian ${KVMDIR}/debian
	cd ${KVMDIR}; dpkg-buildpackage -rfakeroot -us -uc
	lintian ${KVM_DEB} || true

.PHONY: upload
upload: ${KVM_DEB} ${KVMDIR}-src.tar.gz
	umount /pve/${RELEASE}; mount /pve/${RELEASE} -o rw 
	mkdir -p /pve/${RELEASE}/extra
	mkdir -p /pve/${RELEASE}/install
	rm -rf /pve/${RELEASE}/extra/Packages*
	rm -rf /pve/${RELEASE}/extra/${KVMPACKAGE}_*.deb
	rm -rf /pve/${RELEASE}/install/${KVMDIR}-src.tar.gz
	cp ${KVM_DEB} /pve/${RELEASE}/extra
	cp  ${KVMDIR}-src.tar.gz /pve/${RELEASE}/install
	cd /pve/${RELEASE}/extra; dpkg-scanpackages . /dev/null | gzip -9c > Packages.gz
	umount /pve/${RELEASE}; mount /pve/${RELEASE} -o ro

.PHONY: distclean
distclean: clean
	rm -rf ${KVMDIR}.org ${KVMDIR}-src.tar.gz


.PHONY: clean
clean:
	rm -rf *~ ${KVMDIR} ${KVMPACKAGE}_*

.PHONY: dinstall
dinstall: ${KVM_DEB}
	dpkg -i ${KVM_DEB}
