Pod::Spec.new do |s|
  s.name             = 'simple_query_ios'
  s.version          = '0.1.0'
  s.summary          = 'iOS implementation for simple_query federated plugin.'
  s.description      = <<-DESC
iOS implementation for simple_query federated plugin.
                       DESC
  s.homepage         = 'https://github.com/simplezen/simple-query'
  s.license          = { :type => 'MIT', :text => 'MIT' }
  s.author           = { 'SimpleZen' => 'support@simplezen.io' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'Flutter'
  s.platform         = :ios, '14.0'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386'
  }
  s.swift_version = '5.0'
end
