# Contribution guide

Thank you for your interest in contributing to the Wardriver.uk project! We allow contributions from anybody but please keep the following things in mind;

## Issues tracker

It is highly recommended to open an issue before starting to make any changes. You should state the changes you would like to see, indicate that you are willing to do the work, and then wait for approval.

If we decide the change is unnecessary or inappropriate, we can let you know before you put effort into the change which will save your time.

Feel free to also add an issue which you cannot work on and then maybe somebody else will implement it for you.

Bug reports on the issue tracker are also highly welcome and appreciated, even if you can't/won't fix the bug.

## Branches

We use the `main` branch for the alpha/development code. Before you start developing, please ensure your local repository is up to date with the latest commits on our `main` branch. Please create a new branch for your changes
with a short but descriptive name, such as `feature-add-fahrenheit-support` and then create a pull request to merge it into `main` once it is complete.

## Pull requests

All code you want to pull into `main` **must** compile successfully, and you should mention any additional requirements in your pull request (such as additional dependencies).

Please ensure you include a summary about what your change should do within the pull request, also include a reference to the issue on the tracker which your PR addresses.

Try to keep your pull request scoped to a single change. Adding 3 new features should result in 3 pull requests. 

## 3rd party software

When adding 3rd party dependencies, please ensure they can be downloaded from the Arduino library manager. Please also keep the number of new dependencies to a minimum.
You must add a reference to the new library in `libraries.txt` so our automated build service knows what to install. It should be in the format of `libraryname@1.1.1` and a specific version should be used.

In addition, `boards.txt` should be configured in a similar way for the board(s) used. For example: `esp32:esp32@2.0.8`

## Other

Feel free to ask questions on the issues tracker if you would like to contribute but are unsure about how.
