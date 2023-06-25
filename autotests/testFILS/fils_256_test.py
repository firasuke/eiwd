#!/usr/bin/python3

import unittest
import sys
import os

sys.path.append('../util')
import iwd
from iwd import IWD
from iwd import NetworkType
from hostapd import HostapdCLI
import testutil

class Test(unittest.TestCase):
    def test_connection_success(self):
        hapd = HostapdCLI(config='ssidFILS-256.conf')

        wd = IWD(True)

        devices = wd.list_devices(1)
        device = devices[0]

        ordered_network = device.get_ordered_network('ssidFILS-256')

        self.assertEqual(ordered_network.type, NetworkType.eap)

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

        ordered_network.network_object.connect()

        condition = 'obj.state == DeviceState.connected'
        wd.wait_for_object_condition(device, condition)

        testutil.test_iface_operstate()
        testutil.test_ifaces_connected(device.name, hapd.ifname)

        device.disconnect()

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

        ordered_network = device.get_ordered_network('ssidFILS-256')

        self.assertEqual(ordered_network.type, NetworkType.eap)

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

        ordered_network.network_object.connect()

        condition = 'obj.state == DeviceState.connected'
        wd.wait_for_object_condition(device, condition)

        testutil.test_iface_operstate()
        testutil.test_ifaces_connected(device.name, hapd.ifname)

        #
        # TODO: If this is failing its likely due to an older hostapd version
        #       not containing commit 7ee814201b72
        #
        hapd.rekey(device.address)

        device.disconnect()

    @classmethod
    def setUpClass(cls):
        IWD.copy_to_storage('ssidFILS-256.8021x')

    @classmethod
    def tearDownClass(cls):
        IWD.clear_storage()

if __name__ == '__main__':
    unittest.main(exit=True)
