# -*- mode: ruby -*-
# vi: set ft=ruby :
Vagrant.configure("2") do |config|
  config.vm.box = "bento/fedora-latest"

  config.vm.provision "shell", inline: <<-SHELL
    dnf install -y cargo curl make ostree-devel podman rust
    echo "cd /vagrant" >> ~vagrant/.bash_profile
  SHELL
end
