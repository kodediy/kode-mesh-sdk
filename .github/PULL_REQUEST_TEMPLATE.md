*By completing this PR sufficiently, you help us review it quicker and keep the release notes honest.*

### Checklist
1. [ ] A title in the commit format, `type(scope): Summary` (*eg. "fix(mesh_aos): Hold the radio in reset before the battery is cut"*)
2. [ ] Related links below (*eg. the issue this PR closes*)
3. [ ] Relevant documentation updated - header comments, README - if the change touches what they say
4. [ ] The four checks run, and green: `tools/host_tests.sh`, `tools/check_layers.sh`, `tools/check_example_pins.sh` and `idf.py -C tests/target build`
5. [ ] The branch is named for the change: `feat/`, `fix/`, `hotfix/`, `docs/`, ...

*This entire section above can be deleted if all items are checked.*

-----------
## Description of Change
Please describe the proposed change and its impact.

## Test Scenarios
Please describe on what hardware and software this was tested, and how. Tell "seen on a module" from "read in the code".

(*eg. the nine host suites; tests/target on v6.1; a Mesh module on a Kode Dot, USB cable, a 50-ohm load on J501, the GNSS antenna by a window*)

## Related links
Please provide links to related issues, PRs etc.

(*eg. Closes #number of issue*)
