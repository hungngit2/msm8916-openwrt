'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require ui';

return view.extend({
    load: function() {
        return Promise.all([
            uci.load('usbgadget'),
            L.resolveDefault(fs.list('/sys/class/udc'), [])
        ]);
    },

    render: function(data) {
        var udc_devices = data[1];
        var m, s, o;

        m = new form.Map('usbgadget', 
            _('USB Gadget Configuration'),
            _('Configure USB gadget functions. Changes take effect after saving and restarting the service.'));

        // =================================================================
        // Device Settings
        // =================================================================
        s = m.section(form.TypedSection, 'device', _('Device Settings'));
        s.anonymous = true;
        s.addremove = false;

        o = s.option(form.Flag, 'enabled', _('Enable USB Gadget'));
        o.description = _('Disable to use USB port in host mode.') + '<br>' +
            '<strong>' + _('Ensure alternative access.') + '</strong>';
        o.rmempty = false;

        o = s.option(form.Value, 'gadget_name', _('Gadget Name'));
        o.placeholder = 'g1';

        o = s.option(form.Value, 'manufacturer', _('Manufacturer'));
        o.placeholder = 'OpenWrt';

        o = s.option(form.Value, 'product', _('Product'));
        o.placeholder = 'USB Gadget';

        o = s.option(form.Value, 'vendor_id', _('Vendor ID'));
        o.placeholder = '0x1d6b';
        o.validate = function(section_id, value) {
            if (value && !/^0x[0-9a-fA-F]{4}$/.test(value))
                return _('Must be hex format: 0x1234');
            return true;
        };

        o = s.option(form.Value, 'product_id', _('Product ID'));
        o.placeholder = '0x0104';
        o.validate = function(section_id, value) {
            if (value && !/^0x[0-9a-fA-F]{4}$/.test(value))
                return _('Must be hex format: 0x1234');
            return true;
        };

        if (udc_devices.length > 0) {
            o = s.option(form.ListValue, 'udc_device', _('UDC Device'));
            o.value('', _('Auto-detect'));
            udc_devices.forEach(function(dev) {
                o.value(dev.name, dev.name);
            });
        }

        // =================================================================
        // RNDIS
        // =================================================================
        s = m.section(form.NamedSection, 'rndis', 'function', 
            _('RNDIS (Windows Ethernet)'),
            _('Windows-compatible USB ethernet. Works on all Windows versions.'));

        o = s.option(form.Flag, 'enabled', _('Enable RNDIS'),
            _('Plug-and-play on Windows. Automatically adds to LAN bridge.'));
        o.rmempty = false;

        // =================================================================
        // ECM
        // =================================================================
        s = m.section(form.NamedSection, 'ecm', 'function', 
            _('ECM (macOS/Linux Ethernet)'),
            _('For macOS ≤10.14 and Linux. Do not enable with NCM.'));

        o = s.option(form.Flag, 'enabled', _('Enable ECM'),
            _('CDC Ethernet for macOS and Linux. Disable RNDIS when using.'));
        o.rmempty = false;

        // =================================================================
        // NCM
        // =================================================================
        s = m.section(form.NamedSection, 'ncm', 'function', 
            _('NCM (Modern Ethernet)'),
            _('Best performance. For Windows 11+, macOS ≥10.15, Linux.'));

        o = s.option(form.Flag, 'enabled', _('Enable NCM'),
            _('Modern high-speed USB ethernet. Best choice for new systems.'));
        o.rmempty = false;

        // =================================================================
        // ACM Serial / Modem Bridge
        // =================================================================
        s = m.section(form.NamedSection, 'acm', 'function', 
            _('ACM (Serial / Modem AT Bridge)'),
            _('Provides CDC ACM serial port on host. Can be used for shell console or bridging AT commands directly to modem for RouterOS LTE control.'));

        o = s.option(form.Flag, 'enabled', _('Enable Serial Port'));
        o.rmempty = false;

        o = s.option(form.ListValue, 'mode', _('Serial Mode'));
        o.value('shell', _('Login Shell (Root CLI Console)'));
        o.value('modem_bridge', _('Modem AT Bridge (RouterOS / Host LTE Control)'));
        o.value('raw', _('Raw TTY (Unbridged)'));
        o.default = 'shell';

        o = s.option(form.Value, 'modem_port', _('Modem AT Device'));
        o.placeholder = '/dev/wwan0at1';
        o.default = '/dev/wwan0at1';
        o.depends('mode', 'modem_bridge');
        o.description = _('Target modem character device to bridge to /dev/ttyGS0 (e.g. /dev/wwan0at1, /dev/wwan0at0).');

        o = s.option(form.Flag, 'shell', _('Enable Login Shell (Legacy)'));
        o.depends('mode', 'shell');
        o.default = '1';

        o = s.option(form.DummyValue, '_info', _('Connection Info'));
        o.rawhtml = true;
        o.cfgvalue = function() {
            return '<strong>RouterOS LTE Passthrough:</strong> Connects /dev/ttyGS0 <-> /dev/wwan0at1 for /interface lte.<br>' +
                   '<strong>Linux/macOS Shell:</strong> screen /dev/ttyACM0 115200<br>' +
                   '<strong>Windows Shell:</strong> Use PuTTY on COM port';
        };

        // =================================================================
        // Mass Storage
        // =================================================================
        s = m.section(form.NamedSection, 'ums', 'function', 
            _('Mass Storage'),
            _('Expose storage image as USB drive. Does not provide access to the device.'));

        o = s.option(form.Flag, 'enabled', _('Enable Mass Storage'));
        o.rmempty = false;

        o = s.option(form.Value, 'image_path', _('Image Path'),
            _('File will be created automatically if it doesn\'t exist.'));
        o.default = '/var/lib/usb-gadget/storage.img';

        o = s.option(form.Value, 'image_size', _('Image Size'),
            _('Used when creating (e.g., 512M, 1G, 2G)'));
        o.placeholder = '512M';
        o.default = '512M';

        o = s.option(form.Flag, 'readonly', _('Read-Only'),
            _('Mount as read-only on the host computer'));
        o.default = '0';

        return m.render();
    },

    handleSaveApply: function(ev, mode) {
        return this.super('handleSaveApply', [ev, mode]).then(function() {
            ui.addNotification(null, 
                E('p', _('Configuration saved. Restarting USB Gadget service...')), 
                'info');
            return fs.exec('/etc/init.d/usb-gadget', ['restart']).then(function() {
                ui.addNotification(null, 
                    E('p', _('USB Gadget service restarted successfully')), 
                    'info');
            });
        });
    }
});

