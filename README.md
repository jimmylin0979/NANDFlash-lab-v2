# NANDFlash-lab-v2

## Getting Started

```bash
git clone https://github.com/jimmylin0979/NANDFlash-lab-v2.git
cd NANDFlash-lba-v2

# compile for the files
./make_ssd
```

## FTL Server

```bash

# clean log (if you want to test power cycling, don't run this command as it will delete the log)
bash clean_log.sh

# create a ssd folder, start a server mounted on it
mkdir /tmp/ssd
./ssd_fuse -d /tmp/ssd/ssd_file
```

## FTL Test

We have two different test cases, namely `ssd_test` and `ssd_test_cache`. The former is responsible for testing all the read, write, and recovery operations before adding the cache (lab1, lab2), while the latter tests all the operations after adding the cache (lab3).

To test power cycling, which involves power loss and recovery, you can follow these steps:

1. Restart the FTL server without calling `clean_log.sh`.
2. Run the same `ssd_test` (or `ssd_test_cache`) command as before. The testing command will internally log the necessary information for synchronized recovery after power loss.

### ssd_test

```bash
# be sure to create a ftl server first
# example of testing ftl for 10000 times, you can change the number by yourself
./ssd_test /tmp/ssd/ssd_file 10000
```

### ssd_test_cache

```bash
# be sure to create a ftl server first
# example of testing ftl for 10000 times, you can change the number by yourself
./ssd_test_cache /tmp/ssd/ssd_file 10000
```

## Roadmap

-   [ ] Optimization: There remain many TODO in our code to optimize our code, e.g., we can write a log if and only if the flag is toggled. We did not implement it due to the time issue.
-   [ ] WAF pattern design: Design the WAF pattern, checking which kind of user writes/erases will cause our FTL to generate too many WAF.

## Acknowledgments

This is a course taught by an industry expert from [PHISON](https://www.phison.com/en/). We are very grateful for the assistance provided by the instructor and TA. They have been instrumental in inspiring us in both coding and conceptual aspects. We sincerely appreciate their guidance and support.
