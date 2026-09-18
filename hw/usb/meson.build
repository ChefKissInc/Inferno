hw_usb_modules = {}

# usb subsystem core
system_ss.add(when: 'CONFIG_USB', if_true: files(
  'bus.c',
  'combined-packet.c',
  'core.c',
  'desc.c',
  'libhw.c',
  'pcap.c',
), if_false: files('bus-stub.c'))

system_ss.add(when: 'CONFIG_APPLE_OTG', if_true: files('apple_otg.c'))
system_ss.add(when: 'CONFIG_APPLE_TYPEC', if_true: files('apple_typec.c'))
system_ss.add(when: 'CONFIG_USB_UPLINK', if_true: files('dev-inferno-remote.c', 'hcd-inferno.c'))

# usb host adapters
system_ss.add(when: 'CONFIG_USB_EHCI', if_true: files('hcd-ehci.c'))
system_ss.add(when: 'CONFIG_USB_EHCI_SYSBUS', if_true: files('hcd-ehci-sysbus.c'))
system_ss.add(when: 'CONFIG_USB_XHCI', if_true: files('hcd-xhci.c'))
system_ss.add(when: 'CONFIG_USB_XHCI_SYSBUS', if_true: files('hcd-xhci-sysbus.c'))
system_ss.add(when: 'CONFIG_USB_DWC2', if_true: files('hcd-dwc2.c'))
system_ss.add(when: 'CONFIG_USB_DWC3', if_true: files('hcd-dwc3.c'))

# emulated usb devices
system_ss.add(when: 'CONFIG_USB_HUB', if_true: files('dev-hub.c'))

# usb redirect
if usbredir.found()
  usbredir_ss = ss.source_set()
  usbredir_ss.add(when: 'CONFIG_USB',
                  if_true: [usbredir, files('redirect.c', 'quirks.c')])
  hw_usb_modules += {'redirect': usbredir_ss}
endif

# usb pass-through
if libusb.found()
  usbhost_ss = ss.source_set()
  usbhost_ss.add(when: ['CONFIG_USB', libusb],
                 if_true: files('host-libusb.c'))
  hw_usb_modules += {'host': usbhost_ss}
endif

modules += { 'hw-usb': hw_usb_modules }
