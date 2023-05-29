# NANDFlash-lab-v2

## Getting Started

```bash
git clone https://github.com/jimmylin0979/NANDFlash-lab-v2.git
cd NANDFlash-lba-v2

# compile for the files
./make_ssd
```

### ssd_test

```bash

# create environment for testing
mkdir /tmp/ssd

# example of testing ssd_fuse 10000 times, you can change the number by yourself
./ssd_test /tmp/ssd/ssd_file 10000
```

## RoadMap

-   [ ] Implement ssd_test_cache.c
