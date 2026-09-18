# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=diag-tools
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Hung Nguyen <hung.nguyen@skedulo.com>

include $(INCLUDE_DIR)/package.mk

DIAG_BINS:=diag_verno_test3 diag_nv_read diag_nv_sweep diag_nv_write diag_efs rpmsg_diag_open

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(foreach bin,$(DIAG_BINS), \
		$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
			-I $(PKG_BUILD_DIR)/include \
			-o $(PKG_BUILD_DIR)/$(bin) $(PKG_BUILD_DIR)/$(bin).c ; \
	)
endef

define Package/diag-tools
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Qualcomm DIAG/EFS2/NV protocol tools over rpmsg
  DEPENDS:=+busybox
endef

define Package/diag-tools/description
Native DIAG protocol clients speaking to the modem's DIAG channel over an
rpmsg character device (/dev/rpmsg_ctrlN, "DIAG"). Covers classic numbered
NV items (diag_nv_read/diag_nv_write/diag_nv_sweep), the EFS2 file-based NV
subsystem used for IMS/VoLTE config (diag_efs), a firmware-version sanity
probe (diag_verno_test3), the underlying rpmsg endpoint-open primitive
(rpmsg_diag_open), plus two shell helpers: at_cmd.sh (send one AT command
directly to the modem's AT port, bypassing ModemManager) and change_imei.sh
(rewrite NV_IMEI_I / item 550 via diag_nv_write, since AT+EGMR is rejected
on this firmware). Built for the UF896 VoLTE/USSD/RCS investigation; see
Docs/Modem Stability/uf896-diag-tools/README.md for full protocol notes and
usage. Read/write access to modem NV storage -- use deliberately.
endef

define Package/diag-tools/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(foreach bin,$(DIAG_BINS), \
		$(INSTALL_BIN) $(PKG_BUILD_DIR)/$(bin) $(1)/usr/bin/$(bin) ; \
	)
	$(INSTALL_BIN) ./files/at_cmd.sh $(1)/usr/bin/at_cmd.sh
	$(INSTALL_BIN) ./files/change_imei.sh $(1)/usr/bin/change_imei.sh
endef

$(eval $(call BuildPackage,diag-tools))
