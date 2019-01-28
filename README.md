# tcppipe

## HOW
1. follow [this](http://www.petenetlive.com/KB/Article/0001039) step 1-5.
2. according [this](https://kb.vmware.com/s/article/2004954), change step 5 `telnet://<ip>:<port>` to `tcp://<ip>:<port>`.
3. use [tcppipe](https://github.com/kkHAIKE/tcppipe/releases/download/0.1/tcppipe.exe), `tcppipe <ip> <port>`(ip/port is ur ESX srv equal prev step ), the tool create `\\.\pipe\com_1` named pipe that redirect stream to the virtual serial over the network.
4. enjoy kd like vmware workstation local.
