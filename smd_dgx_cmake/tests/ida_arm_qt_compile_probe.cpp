// Match the IDA module's definitions/options, but include Qt before any SDK
// header: generated moc sources do this and cannot rely on SDK intrinsics.
#include <QtCore/qyieldcpu.h>

void ida_arm_qt_compile_probe()
{
    QT_PREPEND_NAMESPACE(qYieldCpu)();
}
