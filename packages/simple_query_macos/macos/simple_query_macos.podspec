Pod::Spec.new do |s|
  s.name             = 'simple_query_macos'
  s.version          = '0.1.0'
  s.summary          = 'macOS implementation for simple_query federated plugin.'
  s.description      = <<-DESC
macOS implementation for simple_query federated plugin.
                       DESC
  s.homepage         = 'https://github.com/simplezen/simple-query'
  s.license          = { :type => 'MIT', :text => 'MIT' }
  s.author           = { 'SimpleZen' => 'support@simplezen.io' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'FlutterMacOS'
  s.platform         = :osx, '10.14'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
