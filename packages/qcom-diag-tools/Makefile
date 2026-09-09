# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=qcom-diag-tools
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -I$(PKG_BUILD_DIR)/include \
		-o $(PKG_BUILD_DIR)/diag_efs $(PKG_BUILD_DIR)/diag_efs.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -I$(PKG_BUILD_DIR)/include \
		-o $(PKG_BUILD_DIR)/diag_nv_read $(PKG_BUILD_DIR)/diag_nv_read.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -I$(PKG_BUILD_DIR)/include \
		-o $(PKG_BUILD_DIR)/diag_nv_write $(PKG_BUILD_DIR)/diag_nv_write.c
endef

define Package/qcom-diag-tools
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Qualcomm DIAG and EFS2 NV diagnostic & provisioning utilities
  DEPENDS:=+busybox
endef

define Package/qcom-diag-tools/description
Provides Qualcomm DIAG and EFS2 file-based NV read/write utilities (diag_efs, diag_nv_read, diag_nv_write)
over rpmsg SMD channels, automated boot-time provisioning for Combined Attach / SMS over SGs, and
standalone USSD querying.
endef

define Package/qcom-diag-tools/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/diag_efs $(1)/usr/sbin/diag_efs
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/diag_nv_read $(1)/usr/sbin/diag_nv_read
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/diag_nv_write $(1)/usr/sbin/diag_nv_write
	$(INSTALL_BIN) ./files/qcom-efs-provision.sh $(1)/usr/sbin/qcom-efs-provision
	$(INSTALL_BIN) ./files/ussd-query.sh $(1)/usr/sbin/ussd-query
	ln -sf ussd-query $(1)/usr/sbin/send-ussd
	$(INSTALL_BIN) ./files/qcom-efs-provision.init $(1)/etc/init.d/qcom-efs-provision
endef

$(eval $(call BuildPackage,qcom-diag-tools))
