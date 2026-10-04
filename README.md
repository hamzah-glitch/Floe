# Floe
Floe is a compiled, statically-typed data pipeline language. The central idea is that data flows left to right through named transformation stages using the |> operator — making programs read the same way they execute.

- - Install Flex, Bison, LLVM 14 or 16

    1. [X] Clean the System Package Repositories

        ```
        sudo rm -f /etc/apt/sources.list.d/*llvm*
        sudo rm -f /etc/apt/trusted.gpg.d/*llvm*
        sudo apt update
        ```
    2. [X] Install Bison, Flex, and LLVM 16 Directly

        ```
        sudo apt install -y bison flex clang-16 llvm-16 lld-16 lldb-16
        ```
    3. [X] Link them as System Defaults

        ```
        sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-16 100
        sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-16 100
        sudo update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-16 100
        ```

    - [X] Check Versions:

    ```
    bison --version && flex --version && clang --version && llvm-config --version
    ```
- [ ]
