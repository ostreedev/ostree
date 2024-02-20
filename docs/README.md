<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

This documentation is written in [Jekyll](https://jekyllrb.com/) format
to be published on [GitHub Pages](https://pages.github.com/). The
rendered HTML will be automatically built and published, but you can
also use Jekyll locally to test changes.

First you need to install [Ruby](https://www.ruby-lang.org/en/) and
[RubyGems](https://rubygems.org/) to get Jekyll and the other gem
dependencies. This is easiest using the distro's packages. On RedHat
systems this is `rubygems` and on Debian systems this is
`ruby-rubygems`.

Next [Bundler](https://bundler.io/) is needed to install the gems using
the provided [Gemfile](Gemfile). You can do this by running `gem install
bundler` or using distro packages. On RedHat systems this is
`rubygem-bundler` and on Debian systems this is `ruby-bundler`.

Now you can prepare the Jekyll environment. Change to this directory and
run:

```
bundle config set --local path vendor/bundle
bundle install
```

Finally, run the `prep-docs.sh` script and then render and serve the
site locally with Jekyll:

```
./prep-docs.sh
bundle exec jekyll serve
```
