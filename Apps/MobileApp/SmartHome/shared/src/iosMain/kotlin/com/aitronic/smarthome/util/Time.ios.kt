package com.aitronic.smarthome.util

import platform.Foundation.NSDate
import platform.Foundation.NSDateFormatter
import platform.Foundation.dateWithTimeIntervalSince1970
import platform.Foundation.timeIntervalSince1970

actual fun nowEpochSeconds(): Long = NSDate().timeIntervalSince1970.toLong()

actual fun formatLocalDateTime(epochSeconds: Long): String {
    val fmt = NSDateFormatter()
    fmt.dateFormat = "HH:mm dd-MM-yyyy"
    return fmt.stringFromDate(NSDate.dateWithTimeIntervalSince1970(epochSeconds.toDouble()))
}

actual fun formatLocalHm(epochSeconds: Long): String {
    val fmt = NSDateFormatter()
    fmt.dateFormat = "HH:mm"
    return fmt.stringFromDate(NSDate.dateWithTimeIntervalSince1970(epochSeconds.toDouble()))
}
