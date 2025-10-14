FROM fedora:42

RUN sudo dnf install -y ninja-build cmake clang lld liburing liburing-devel \
libcxx libcxx-devel git git-lfs gnupg2 ccache iproute wireshark-cli btop unzip

RUN wget https://github.com/grafana/pyroscope/releases/download/v1.15.0/pyroscope_1.15.0_linux_amd64.tar.gz \
&& mkdir pyroscope \
&& tar xf pyroscope_1.15.0_linux_amd64.tar.gz -C pyroscope \
&& rm pyroscope_1.15.0_linux_amd64.tar.gz

RUN wget https://github.com/grafana/alloy/releases/download/v1.11.2/alloy-linux-amd64.zip \
&& mkdir alloy \
&& unzip alloy-linux-amd64.zip -d alloy \
&& rm alloy-linux-amd64.zip \
&& chmod +x /alloy/alloy-linux-amd64