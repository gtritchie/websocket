We welcome contributions to the **websocket** package. To submit a contribution:

1. [Fork](https://github.com/rstudio/websocket/fork) the repository and make your changes.

2. Ensure that you have signed the [individual](https://www.rstudio.com/wp-content/uploads/2014/06/rstudioindividualcontributoragreement.pdf) or [corporate](https://www.rstudio.com/wp-content/uploads/2014/06/rstudiocorporatecontributoragreement.pdf) contributor agreement as appropriate. You can send the signed copy to contribute@rstudio.com.

3. Submit a [pull request](https://help.github.com/articles/using-pull-requests).

We generally will not merge a pull request that updates `websocketpp` or included libraries because it is difficult for us to verify that the update is done correctly. We prefer to update included libraries ourselves.

## How to make changes

Before you submit a pull request, please do the following:

* Add an entry to NEWS.md concisely describing what you changed.

* If appropriate, add unit tests in the tests/ directory.

* Run Build->Check Package in the RStudio IDE, or `devtools::check()`, to make sure your change did not add any messages, warnings, or errors.

Doing these things will make it easier to evaluate your pull request. Even so, we may still decide to modify your code or even not merge it at all. Factors that may prevent us from merging the pull request include:

* breaking backward compatibility
* adding a feature that we do not consider relevant
* is hard to understand
* is hard to maintain in the future
* is computationally expensive
* is not intuitive for people to use

We will try to be responsive and provide feedback in case we decide not to merge your pull request.

## Filing issues

If you find a bug in websocket, you can also [file an issue](https://github.com/rstudio/websocket/issues/new). Please provide as much relevant information as you can, and include a minimal reproducible example if possible.
