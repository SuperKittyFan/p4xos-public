# Acknowledgement

This repository is the fork of [P4xos from usi-systems](https://github.com/usi-systems/p4xos-public) for the possible improvements on p4paxos and some exploration on the performance measurements. This is UIUC 538 Advance Computer Networks Research Project.

# [Introduction (see our poster)](https://docs.google.com/presentation/d/1eAVVu2V8FlbMpEaQDPW3wm-7gpuBA5DU9PvsoWlqyiY/edit?usp=sharing)

# Setup a Virtual Machine

* `cd environment`
* `vagrant up` (should install vagrant and virtualbox)

# Demo

After connecting to the VM (`vagrant ssh`), cd to the **p4paxos-public/bmv2** directory, and start the demo

```
cd p4paxos/bmv2
sudo ./run_demo.sh
```

From the mininet prompt

```
mininet> xterm h4 (should install xquartz for macOS to enable the display forwarding)
```

# Testing
in h4 terminal
```
./scripts/test.sh
```
Or, start Firefox web browser in h4 terminal
```
firefox &
```

In the firefox browser, visit 10.0.0.1:8080 , then you can try to Get or Put a (key, value) pair.

# Simulating Failures

In this demo, there are two replicas (learners) running on h2 and h3.
The service is still alive if any one of switches or replicas crash.

We can simulate the link failure by running *link* command in Mininet
```
mininet> link h2 s2 down
```

Or, simulate the server failure by stopping the server process
```
mininet> h2 kill %python
```

## Config

In *bmv2/scripts/paxos.cfg* configuration file:

```
[instance]
count=65536

[timeout]
second=0
```

* The *count* variable in the *instance* section: configures the maximum 
number of requests that learners will handle.

* The *second* variable in *timeout* section: configures the number of seconds
that the learners will stay alive. If timeout is set to 0, the learners forever.
