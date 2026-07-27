#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"

int main()
{
    hakoniwa::pdu::rpc::RpcServicesClient client(
        "package-consumer-node",
        "PackageConsumer",
        "unused-service-config.json");
    return client.start_all_services() ? 1 : 0;
}
