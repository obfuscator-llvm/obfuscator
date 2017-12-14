// RUN: %clang_analyze_cc1 -analyzer-output=plist -analyzer-config notes-as-events=true -o %t.plist -std=c++11 -analyzer-checker=alpha.clone.CloneChecker -verify %s
// RUN: FileCheck --input-file=%t.plist %s

void log();

int max(int a, int b) { // expected-warning{{Duplicate code detected}}
  log();
  if (a > b)
    return a;
  return b;
}

int maxClone(int a, int b) { // no-note (converted into event)
  log();
  if (a > b)
    return a;
  return b;
}

// CHECK:  <key>diagnostics</key>
// CHECK-NEXT:  <array>
// CHECK-NEXT:   <dict>
// CHECK-NEXT:    <key>path</key>
// CHECK-NEXT:    <array>
// CHECK-NEXT:     <dict>
// CHECK-NEXT:      <key>kind</key><string>event</string>
// CHECK-NEXT:      <key>location</key>
// CHECK-NEXT:      <dict>
// CHECK-NEXT:       <key>line</key><integer>13</integer>
// CHECK-NEXT:       <key>col</key><integer>28</integer>
// CHECK-NEXT:       <key>file</key><integer>0</integer>
// CHECK-NEXT:      </dict>
// CHECK-NEXT:      <key>ranges</key>
// CHECK-NEXT:      <array>
// CHECK-NEXT:        <array>
// CHECK-NEXT:         <dict>
// CHECK-NEXT:          <key>line</key><integer>13</integer>
// CHECK-NEXT:          <key>col</key><integer>28</integer>
// CHECK-NEXT:          <key>file</key><integer>0</integer>
// CHECK-NEXT:         </dict>
// CHECK-NEXT:         <dict>
// CHECK-NEXT:          <key>line</key><integer>18</integer>
// CHECK-NEXT:          <key>col</key><integer>1</integer>
// CHECK-NEXT:          <key>file</key><integer>0</integer>
// CHECK-NEXT:         </dict>
// CHECK-NEXT:        </array>
// CHECK-NEXT:      </array>
// CHECK-NEXT:      <key>depth</key><integer>0</integer>
// CHECK-NEXT:      <key>extended_message</key>
// CHECK-NEXT:      <string>Similar code here</string>
// CHECK-NEXT:      <key>message</key>
// CHECK-NEXT:      <string>Similar code here</string>
// CHECK-NEXT:     </dict>
// CHECK-NEXT:     <dict>
// CHECK-NEXT:      <key>kind</key><string>event</string>
// CHECK-NEXT:      <key>location</key>
// CHECK-NEXT:      <dict>
// CHECK-NEXT:       <key>line</key><integer>6</integer>
// CHECK-NEXT:       <key>col</key><integer>23</integer>
// CHECK-NEXT:       <key>file</key><integer>0</integer>
// CHECK-NEXT:      </dict>
// CHECK-NEXT:      <key>ranges</key>
// CHECK-NEXT:      <array>
// CHECK-NEXT:        <array>
// CHECK-NEXT:         <dict>
// CHECK-NEXT:          <key>line</key><integer>6</integer>
// CHECK-NEXT:          <key>col</key><integer>23</integer>
// CHECK-NEXT:          <key>file</key><integer>0</integer>
// CHECK-NEXT:         </dict>
// CHECK-NEXT:         <dict>
// CHECK-NEXT:          <key>line</key><integer>11</integer>
// CHECK-NEXT:          <key>col</key><integer>1</integer>
// CHECK-NEXT:          <key>file</key><integer>0</integer>
// CHECK-NEXT:         </dict>
// CHECK-NEXT:        </array>
// CHECK-NEXT:      </array>
// CHECK-NEXT:      <key>depth</key><integer>0</integer>
// CHECK-NEXT:      <key>extended_message</key>
// CHECK-NEXT:      <string>Duplicate code detected</string>
// CHECK-NEXT:      <key>message</key>
// CHECK-NEXT:      <string>Duplicate code detected</string>
// CHECK-NEXT:     </dict>
// CHECK-NEXT:    </array>
// CHECK-NEXT:    <key>description</key><string>Duplicate code detected</string>
// CHECK-NEXT:    <key>category</key><string>Code clone</string>
// CHECK-NEXT:    <key>type</key><string>Exact code clone</string>
// CHECK-NEXT:    <key>check_name</key><string>alpha.clone.CloneChecker</string>
// CHECK-NEXT:    <!-- This hash is experimental and going to change! -->
// CHECK-NEXT:    <key>issue_hash_content_of_line_in_context</key><string>3d15184f38c5fa57e479b744fe3f5035</string>
// CHECK-NEXT:   <key>location</key>
// CHECK-NEXT:   <dict>
// CHECK-NEXT:    <key>line</key><integer>6</integer>
// CHECK-NEXT:    <key>col</key><integer>23</integer>
// CHECK-NEXT:    <key>file</key><integer>0</integer>
// CHECK-NEXT:   </dict>
// CHECK-NEXT:   </dict>
// CHECK-NEXT:  </array>
