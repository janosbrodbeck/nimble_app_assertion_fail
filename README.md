<!--
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
#  KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
-->

# NimBLE extended advertising - assertion fail test app

### Building / flashing
Targets for nrf52dk and nrf52840dk are already provided.  
(For nrf52840dk replace nrf52 with nrf52840 in target names)

* Prepare repo `newt upgrade`
* Build bootloader: `newt build nrf52_boot`
* Build app: `newt build nrf52_ext_app`
* Create image: `newt create-image nrf52_ext_app 1.0.0`
* Load bootloader: `newt load nrf52_boot`
* Load app `newt load nrf52_ext_app`

Open terminal (e.g. `pyterm -p /dev/tty...`).

### Use the app
Flash the application on two nodes. One will serve as consumer the other as producer.

__Producer:__ Start with `producer <interval>`, interval given in ms. Recommended values: 200 to 500 as starting points. This should trigger the issue very fast.  
__Consumer:__ Start with entering `consumer` into the console  

The lower, the faster the crash can be triggered. Would not recommend to go below 200/250ms.

### About the producer/consumer:

__Producer__ produces marked packets every \<interval\> ms and scans for packets (processes them but does not advertise them back again).  
__Consumer__ just scans for marked packets and advertises them back without any changes.  
