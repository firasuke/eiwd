#! /usr/bin/python3

import unittest
import sys, os

sys.path.append('../util')
from iwd import IWD
from iwd import NetworkType
from hostapd import HostapdCLI
import testutil

class Test(unittest.TestCase):
    def test_preauth_success(self):
        bss_hostapd = [ HostapdCLI(config='eaptls-preauth-1.conf'),
                        HostapdCLI(config='eaptls-preauth-2.conf') ]

        bss0_addr = bss_hostapd[0].bssid
        bss1_addr = bss_hostapd[1].bssid

        # Fill in the neighbor AP tables in both BSSes.  By default each
        # instance knows only about current BSS, even inside one hostapd
        # process.
        # Roaming still works without the neighbor AP table but neighbor
        # reports have to be disabled in the .conf files
        bss0_nr = ''.join(bss0_addr.split(':')) + \
                '8f0000005101060603000000'
        bss1_nr = ''.join(bss1_addr.split(':')) + \
                '8f0000005102060603000000'

        bss_hostapd[0].set_neighbor(bss1_addr, 'TestPreauth', bss1_nr)
        bss_hostapd[1].set_neighbor(bss0_addr, 'TestPreauth', bss0_nr)

        wd = IWD(True)

        device = wd.list_devices(1)[0]

        ordered_network = device.get_ordered_network('TestPreauth')

        self.assertEqual(ordered_network.type, NetworkType.eap)

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

        self.assertFalse(bss_hostapd[0].list_sta())
        self.assertFalse(bss_hostapd[1].list_sta())

        device.connect_bssid(bss0_addr)

        condition = 'obj.state == DeviceState.connected'
        wd.wait_for_object_condition(device, condition)

        self.assertTrue(bss_hostapd[0].list_sta())
        self.assertFalse(bss_hostapd[1].list_sta())

        testutil.test_iface_operstate(device.name)
        testutil.test_ifaces_connected(bss_hostapd[0].ifname, device.name)
        self.assertRaises(Exception, testutil.test_ifaces_connected,
                          bss_hostapd[1].ifname, device.name, True, True)

        device.roam(bss1_addr)

        condition = 'obj.state == DeviceState.roaming'
        wd.wait_for_object_condition(device, condition)

        # TODO: verify that the PMK from preauthentication was used

        # Check that iwd is on BSS 1 once out of roaming state and doesn't
        # go through 'disconnected', 'autoconnect', 'connecting' in between
        from_condition = 'obj.state == DeviceState.roaming'
        to_condition = 'obj.state == DeviceState.connected'
        wd.wait_for_object_change(device, from_condition, to_condition)

        self.assertTrue(bss_hostapd[1].list_sta())

        testutil.test_iface_operstate(device.name)
        testutil.test_ifaces_connected(bss_hostapd[1].ifname, device.name)
        self.assertRaises(Exception, testutil.test_ifaces_connected,
                          (bss_hostapd[0].ifname, device.name, True, True))

        device.disconnect()

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

    @classmethod
    def setUpClass(cls):
        IWD.copy_to_storage('TestPreauth.8021x')

        os.system('ifconfig lo up')

    @classmethod
    def tearDownClass(cls):
        IWD.clear_storage()

if __name__ == '__main__':
    unittest.main(exit=True)
